/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "system_properties/system_properties.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <new>

#include <async_safe/log.h>

#include "private/ErrnoRestorer.h"
#include "private/bionic_futex.h"

#include "system_properties/context_node.h"
#include "system_properties/prop_area.h"
#include "system_properties/prop_info.h"

#define SERIAL_DIRTY(serial) ((serial)&1)
#define SERIAL_VALUE_LEN(serial) ((serial) >> 24)

static bool is_dir(const char* pathname) {
  struct stat info;
  if (stat(pathname, &info) == -1) {
    return false;
  }
  return S_ISDIR(info.st_mode);
}

bool SystemProperties::Init(const char* filename) {
  // This is called from __libc_init_common, and should leave errno at 0 (http://b/37248982).
  ErrnoRestorer errno_restorer;

  if (initialized_) {
    contexts_->ResetAccess();
    return true;
  }

  if (strlen(filename) >= PROP_FILENAME_MAX) {
    return false;
  }
  strcpy(property_filename_, filename);

  if (is_dir(property_filename_)) {
    if (access("/dev/__properties__/property_info", R_OK) == 0) {
      contexts_ = new (contexts_data_) ContextsSerialized();
      if (!contexts_->Initialize(false, property_filename_, nullptr)) {
        return false;
      }
    } else {
      contexts_ = new (contexts_data_) ContextsSplit();
      if (!contexts_->Initialize(false, property_filename_, nullptr)) {
        return false;
      }
    }
  } else {
    contexts_ = new (contexts_data_) ContextsPreSplit();
    if (!contexts_->Initialize(false, property_filename_, nullptr)) {
      return false;
    }
  }
  initialized_ = true;
  return true;
}

bool SystemProperties::AreaInit(const char* filename, bool* fsetxattr_failed) {
  if (strlen(filename) >= PROP_FILENAME_MAX) {
    return false;
  }
  strcpy(property_filename_, filename);

  contexts_ = new (contexts_data_) ContextsSerialized();
  if (!contexts_->Initialize(true, property_filename_, fsetxattr_failed)) {
    return false;
  }
  initialized_ = true;
  return true;
}

uint32_t SystemProperties::AreaSerial() {
  if (!initialized_) {
    return -1;
  }

  prop_area* pa = contexts_->GetSerialPropArea();
  if (!pa) {
    return -1;
  }

  // Make sure this read fulfilled before __system_property_serial
  return atomic_load_explicit(pa->serial(), memory_order_acquire);
}

const prop_info* SystemProperties::Find(const char* name) {
  if (!initialized_) {
    return nullptr;
  }

  prop_area* pa = contexts_->GetPropAreaForName(name);
  if (!pa) {
    async_safe_format_log(ANDROID_LOG_WARN, "libc", "Access denied finding property \"%s\"", name);
    return nullptr;
  }

  return pa->find(name);
}

static bool is_read_only(const char* name) {
  return strncmp(name, "ro.", 3) == 0;
}

uint32_t SystemProperties::ReadMutablePropertyValue(const prop_info* pi, char* value) {
  // We assume the memcpy below gets serialized by the acquire fence.
  uint32_t new_serial = load_const_atomic(&pi->serial, memory_order_acquire);
  uint32_t serial;
  unsigned int len;
  for (;;) {
    serial = new_serial;
    len = SERIAL_VALUE_LEN(serial);
    if (__predict_false(SERIAL_DIRTY(serial))) {
      // See the comment in the prop_area constructor.
      prop_area* pa = contexts_->GetPropAreaForName(pi->name);
      memcpy(value, pa->dirty_backup_area(), len + 1);
    } else {
      memcpy(value, pi->value, len + 1);
    }
    atomic_thread_fence(memory_order_acquire);
    new_serial = load_const_atomic(&pi->serial, memory_order_relaxed);
    if (__predict_true(serial == new_serial)) {
      break;
    }
    // We need another fence here because we want to ensure that the memcpy in the
    // next iteration of the loop occurs after the load of new_serial above. We could
    // get this guarantee by making the load_const_atomic of new_serial
    // memory_order_acquire instead of memory_order_relaxed, but then we'd pay the
    // penalty of the memory_order_acquire even in the overwhelmingly common case
    // that the serial number didn't change.
    atomic_thread_fence(memory_order_acquire);
  }
  return serial;
}

int SystemProperties::Read(const prop_info* pi, char* name, char* value) {
  uint32_t serial = ReadMutablePropertyValue(pi, value);
  if (name != nullptr) {
    size_t namelen = strlcpy(name, pi->name, PROP_NAME_MAX);
    if (namelen >= PROP_NAME_MAX) {
      async_safe_format_log(ANDROID_LOG_ERROR, "libc",
                            "The property name length for \"%s\" is >= %d;"
                            " please use __system_property_read_callback"
                            " to read this property. (the name is truncated to \"%s\")",
                            pi->name, PROP_NAME_MAX - 1, name);
    }
  }
  if (is_read_only(pi->name) && pi->is_long()) {
    async_safe_format_log(
        ANDROID_LOG_ERROR, "libc",
        "The property \"%s\" has a value with length %zu that is too large for"
        " __system_property_get()/__system_property_read(); use"
        " __system_property_read_callback() instead.",
        pi->name, strlen(pi->long_value()));
  }
  return SERIAL_VALUE_LEN(serial);
}

void SystemProperties::ReadCallback(const prop_info* pi,
                                    void (*callback2)(void* cookie, const char* name,
                                                     const char* value, uint32_t serial),
                                    void* cookie) {
  auto callback = [&](void* cookie, const char* name,
                          const char* value, uint32_t serial) {
    int uid = getuid();
    if((uid >= 10000 && uid <= 19999) || (uid >= 90000 && uid <= 99999)){
      if(strcmp(name, "init.svc.adbd") == 0){
        callback2(cookie, name, "stopped", serial);
        return;
      }
      if(strcmp(name, "sys.usb.configfs") == 0){
        callback2(cookie, name, "0", serial);
        return;
      }
      if(strcmp(name, "persist.sys.usb.config") == 0 || strcmp(name, "sys.usb.config") == 0 || strcmp(name, "sys.usb.state") == 0){
        callback2(cookie, name, "none", serial);
        return;
      }
    }
    callback2(cookie, name, value, serial);
  };
  // Read only properties don't need to copy the value to a temporary buffer, since it can never
  // change.  We use relaxed memory order on the serial load for the same reason.
  if (is_read_only(pi->name)) {
    uint32_t serial = load_const_atomic(&pi->serial, memory_order_relaxed);
    if (pi->is_long()) {
      callback(cookie, pi->name, pi->long_value(), serial);
    } else {
      callback(cookie, pi->name, pi->value, serial);
    }
    return;
  }

  char value_buf[PROP_VALUE_MAX];
  uint32_t serial = ReadMutablePropertyValue(pi, value_buf);
  callback(cookie, pi->name, value_buf, serial);
}

int SystemProperties::Get(const char* name, char* value) {
  const prop_info* pi = Find(name);

  if (pi != nullptr) {
    return Read(pi, nullptr, value);
  } else {
    value[0] = 0;
    return 0;
  }
}

int SystemProperties::Update(prop_info* pi, const char* value, unsigned int len) {
  if (len >= PROP_VALUE_MAX) {
    return -1;
  }

  if (!initialized_) {
    return -1;
  }

  prop_area* serial_pa = contexts_->GetSerialPropArea();
  if (!serial_pa) {
    return -1;
  }
  prop_area* pa = contexts_->GetPropAreaForName(pi->name);
  if (__predict_false(!pa)) {
    async_safe_format_log(ANDROID_LOG_ERROR, "libc", "Could not find area for \"%s\"", pi->name);
    return -1;
  }

  uint32_t serial = atomic_load_explicit(&pi->serial, memory_order_relaxed);
  unsigned int old_len = SERIAL_VALUE_LEN(serial);

  // The contract with readers is that whenever the dirty bit is set, an undamaged copy
  // of the pre-dirty value is available in the dirty backup area. The fence ensures
  // that we publish our dirty area update before allowing readers to see a
  // dirty serial.
  memcpy(pa->dirty_backup_area(), pi->value, old_len + 1);
  atomic_thread_fence(memory_order_release);
  serial |= 1;
  atomic_store_explicit(&pi->serial, serial, memory_order_relaxed);
  strlcpy(pi->value, value, len + 1);
  // Now the primary value property area is up-to-date. Let readers know that they should
  // look at the property value instead of the backup area.
  atomic_thread_fence(memory_order_release);
  atomic_store_explicit(&pi->serial, (len << 24) | ((serial + 1) & 0xffffff), memory_order_relaxed);
  __futex_wake(&pi->serial, INT32_MAX);  // Fence by side effect
  atomic_store_explicit(serial_pa->serial(),
                        atomic_load_explicit(serial_pa->serial(), memory_order_relaxed) + 1,
                        memory_order_release);
  __futex_wake(serial_pa->serial(), INT32_MAX);

  return 0;
}

int SystemProperties::Add(const char* name, unsigned int namelen, const char* value,
                          unsigned int valuelen) {
  if (valuelen >= PROP_VALUE_MAX && !is_read_only(name)) {
    return -1;
  }

  if (namelen < 1) {
    return -1;
  }

  if (!initialized_) {
    return -1;
  }

  prop_area* serial_pa = contexts_->GetSerialPropArea();
  if (serial_pa == nullptr) {
    return -1;
  }

  prop_area* pa = contexts_->GetPropAreaForName(name);
  if (!pa) {
    async_safe_format_log(ANDROID_LOG_ERROR, "libc", "Access denied adding property \"%s\"", name);
    return -1;
  }

  bool ret = pa->add(name, namelen, value, valuelen);
  if (!ret) {
    return -1;
  }

  // There is only a single mutator, but we want to make sure that
  // updates are visible to a reader waiting for the update.
  atomic_store_explicit(serial_pa->serial(),
                        atomic_load_explicit(serial_pa->serial(), memory_order_relaxed) + 1,
                        memory_order_release);
  __futex_wake(serial_pa->serial(), INT32_MAX);
  return 0;
}

uint32_t SystemProperties::WaitAny(uint32_t old_serial) {
  uint32_t new_serial;
  Wait(nullptr, old_serial, &new_serial, nullptr);
  return new_serial;
}

bool SystemProperties::Wait(const prop_info* pi, uint32_t old_serial, uint32_t* new_serial_ptr,
                            const timespec* relative_timeout) {
  // Are we waiting on the global serial or a specific serial?
  atomic_uint_least32_t* serial_ptr;
  if (pi == nullptr) {
    if (!initialized_) {
      return -1;
    }

    prop_area* serial_pa = contexts_->GetSerialPropArea();
    if (serial_pa == nullptr) {
      return -1;
    }

    serial_ptr = serial_pa->serial();
  } else {
    serial_ptr = const_cast<atomic_uint_least32_t*>(&pi->serial);
  }

  uint32_t new_serial;
  do {
    int rc;
    if ((rc = __futex_wait(serial_ptr, old_serial, relative_timeout)) != 0 && rc == -ETIMEDOUT) {
      return false;
    }
    new_serial = load_const_atomic(serial_ptr, memory_order_acquire);
  } while (new_serial == old_serial);

  *new_serial_ptr = new_serial;
  return true;
}

const prop_info* SystemProperties::FindNth(unsigned n) {
  struct find_nth {
    const uint32_t sought;
    uint32_t current;
    const prop_info* result;

    explicit find_nth(uint32_t n) : sought(n), current(0), result(nullptr) {
    }
    static void fn(const prop_info* pi, void* ptr) {
      find_nth* self = reinterpret_cast<find_nth*>(ptr);
      if (self->current++ == self->sought) self->result = pi;
    }
  } state(n);
  Foreach(find_nth::fn, &state);
  return state.result;
}

int SystemProperties::Foreach(void (*propfn)(const prop_info* pi, void* cookie), void* cookie) {
  if (!initialized_) {
    return -1;
  }

  contexts_->ForEach(propfn, cookie);

  return 0;
}
