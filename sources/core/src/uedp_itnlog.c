/** ANCHOR - Implementation of internal logger for UEDP
 * @file uedp_itnlog.c
 * @author Shang Huang
 * @version 0.1
 * @date 2026-05-14
 * @copyright MIT License
 */
#include <stdio.h>
#include "uedp_core.h"
#include "uedp_task.h"
#include "uedp_msg.h"
#include "uedp_fsm.h"
#include "uedp_tsm.h"
#include "uedp_itnlog.h"
#include "uedp_ocesvc.h"
#include "libcrc8.h"

#define UEDP_ITNLOG_OUTPUT_LINE_MAX  (128u)

typedef struct internal_uedp_itnlog_slot_t {
  uedp_itnlog_entry_t entry;
  char msg_buf[UEDP_ITNLOG_MSG_MAX_LEN];
  char tag_buf[UEDP_ITNLOG_TAG_MAX_LEN];
} internal_uedp_itnlog_slot_t;

sta internal_uedp_itnlog_slot_t itnlog_slots[UEDP_ITNLOG_MAX_LOG_ENTRIES];
sta ui16 itnlog_head = 0;
sta ui16 itnlog_count = 0;
sta ui16 itnlog_dropped = 0;

sta uedp_itnlog_level_t itnlog_filter_level = ITNLOG_LEVEL_DEBUG;
sta const char* itnlog_filter_tag = NULL;
sta uedp_itnlog_output_func_t itnlog_output_func = NULL;

sta ocesvc_t itnlog_flush_svc;

sta void internal_uedp_itnlog_calc_hash(uedp_itnlog_entry_t* entry);
sta void internal_uedp_itnlog_copy_str(char* dst, size_t dst_size, const char* src);
sta void internal_uedp_itnlog_flush_handler(ocesvc_t* me);

void internal_uedp_itnlog_calc_hash(uedp_itnlog_entry_t* entry) {
  uint8_t checksum = 0;
  uint8_t byte;

  byte = (uint8_t)entry->level;
  checksum = crc8(&byte, 1, checksum);
  byte = (uint8_t)(entry->task_id >> 8);
  checksum = crc8(&byte, 1, checksum);
  byte = (uint8_t)entry->task_id;
  checksum = crc8(&byte, 1, checksum);
  byte = (uint8_t)(entry->msg_sig >> 8);
  checksum = crc8(&byte, 1, checksum);
  byte = (uint8_t)entry->msg_sig;
  checksum = crc8(&byte, 1, checksum);
  byte = (uint8_t)(entry->tmstmp >> 24);
  checksum = crc8(&byte, 1, checksum);
  byte = (uint8_t)(entry->tmstmp >> 16);
  checksum = crc8(&byte, 1, checksum);
  byte = (uint8_t)(entry->tmstmp >> 8);
  checksum = crc8(&byte, 1, checksum);
  byte = (uint8_t)entry->tmstmp;
  checksum = crc8(&byte, 1, checksum);

  for (const uint8_t* cursor = (const uint8_t*)entry->tag; *cursor != '\0'; cursor++) {
    checksum = crc8((uint8_t*)cursor, 1, checksum);
  }
  for (const uint8_t* cursor = (const uint8_t*)entry->msg; *cursor != '\0'; cursor++) {
    checksum = crc8((uint8_t*)cursor, 1, checksum);
  }

  entry->hash = checksum;
}

void internal_uedp_itnlog_copy_str(char* dst, size_t dst_size, const char* src) {
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dst_size - 1U);
  dst[dst_size - 1U] = '\0';
}

void internal_uedp_itnlog_flush_handler(ocesvc_t* me) {
  (void)me;
  uedp_itnlog_dump();
}

void uedp_itnlog_init(void) {
  memset(itnlog_slots, 0, sizeof(itnlog_slots));
  itnlog_head = 0;
  itnlog_count = 0;
  itnlog_dropped = 0;

  itnlog_flush_svc.handler = internal_uedp_itnlog_flush_handler;
  itnlog_flush_svc.context = NULL;
  ocesvc_register(&itnlog_flush_svc);
}

void uedp_itnlog_log(ui32 timestamp, uedp_itnlog_level_t level, const char* tag, const char* msg) {
  if (itnlog_count >= UEDP_ITNLOG_MAX_LOG_ENTRIES) {
    itnlog_dropped++;
    ocesvc_set_ready(&itnlog_flush_svc);
    return;
  }

  uedp_msg_t* cur_msg = uedp_task_norm_get_current_msg();
  ui16 index = (ui16)((itnlog_head + itnlog_count) % UEDP_ITNLOG_MAX_LOG_ENTRIES);
  internal_uedp_itnlog_slot_t* slot = &itnlog_slots[index];

  internal_uedp_itnlog_copy_str(slot->tag_buf, sizeof(slot->tag_buf), tag);
  internal_uedp_itnlog_copy_str(slot->msg_buf, sizeof(slot->msg_buf), msg);

  slot->entry.level = level;
  slot->entry.tag = slot->tag_buf;
  slot->entry.msg = slot->msg_buf;
  slot->entry.task_id = uedp_task_norm_get_current_id();
  slot->entry.msg_sig = (cur_msg != NULL) ? cur_msg->sig : 0x0000; // 0x0000: không có task/msg nào đang dispatch
  slot->entry.tmstmp = timestamp;
  slot->entry.hash = 0;
  internal_uedp_itnlog_calc_hash(&slot->entry);

  itnlog_count++;
  if (itnlog_count >= UEDP_ITNLOG_FLUSH_THRESHOLD) {
    ocesvc_set_ready(&itnlog_flush_svc);
  }
}

uedp_itnlog_entry_t uedp_itnlog_clear(void) {
  if (itnlog_count == 0) {
    return (uedp_itnlog_entry_t){0};
  }

  uedp_itnlog_entry_t entry = itnlog_slots[itnlog_head].entry;
  itnlog_head = (ui16)((itnlog_head + 1U) % UEDP_ITNLOG_MAX_LOG_ENTRIES);
  itnlog_count--;
  return entry;
}

void uedp_itnlog_dump(void) {
  char formatted_entry[UEDP_ITNLOG_OUTPUT_LINE_MAX];

  while (itnlog_count > 0) {
    uedp_itnlog_entry_t entry = uedp_itnlog_clear();
    if (
      entry.level >= itnlog_filter_level &&
      (itnlog_filter_tag == NULL || strcmp(entry.tag, itnlog_filter_tag) == 0)
    ) {
      if (itnlog_output_func != NULL) {
        snprintf(
          formatted_entry,
          sizeof(formatted_entry),
          "[ITNLOG] %lu 0x%02X 0x%02X %s\n",
          (unsigned long)entry.tmstmp,
          (unsigned int)entry.task_id,
          (unsigned int)entry.msg_sig,
          entry.msg
        );
        itnlog_output_func(formatted_entry);
      }
    }
  }

  if (itnlog_dropped > 0 && itnlog_output_func != NULL) {
    snprintf(formatted_entry, sizeof(formatted_entry), "[ITNLOG] dropped %u\n", (unsigned int)itnlog_dropped);
    itnlog_output_func(formatted_entry);
  }
  itnlog_dropped = 0;
}

ui16 uedp_itnlog_get_log_count(void) {
  return itnlog_count;
}

void uedp_itnlog_set_level(uedp_itnlog_level_t level) {
  itnlog_filter_level = level;
}

uedp_itnlog_level_t uedp_itnlog_get_level(void) {
  return itnlog_filter_level;
}

const char* uedp_itnlog_get_tag(void) {
  return itnlog_filter_tag;
}

void uedp_itnlog_set_tag(const char* tag) {
  itnlog_filter_tag = tag;
}

void uedp_itnlog_set_filter(uedp_itnlog_level_t level, const char* tag) {
  uedp_itnlog_set_level(level);
  uedp_itnlog_set_tag(tag);
}

void uedp_itnlog_get_filter(uedp_itnlog_level_t* level, char* tag) {
  if (level != NULL) {
    *level = uedp_itnlog_get_level();
  }
  if (tag != NULL) {
    const char* cur_tag = uedp_itnlog_get_tag();
    *tag = (cur_tag != NULL) ? cur_tag[0] : '\0';
  }
}

void uedp_itnlog_set_output(void (*output_func)(const char*)) {
  itnlog_output_func = output_func;
}
