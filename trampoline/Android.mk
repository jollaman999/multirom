LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_C_INCLUDES += $(multirom_local_path) $(multirom_local_path)/lib
LOCAL_SRC_FILES:= \
    trampoline.c \
    devices.c \
    adb.c \

LOCAL_MODULE:= trampoline
LOCAL_MODULE_TAGS := optional

LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT)
LOCAL_UNSTRIPPED_PATH := $(TARGET_ROOT_OUT_UNSTRIPPED)
LOCAL_STATIC_LIBRARIES := libcutils libc libmultirom_static libbootimg
LOCAL_FORCE_STATIC_EXECUTABLE := true

ifeq ($(MR_INIT_DEVICES),)
    $(info MR_INIT_DEVICES was not defined in device files!)
endif
LOCAL_SRC_FILES += ../../../../$(MR_INIT_DEVICES)

ifneq ($(MR_EXTRA_FIRMWARE_DIR),)
        LOCAL_CFLAGS += -DMR_EXTRA_FIRMWARE_DIR="\"$(MR_EXTRA_FIRMWARE_DIR)\""
endif

# for adb
LOCAL_CFLAGS += -DPRODUCT_MODEL="\"$(PRODUCT_MODEL)\"" -DPRODUCT_MANUFACTURER="\"$(PRODUCT_MANUFACTURER)\""

# to find fstab
LOCAL_CFLAGS += -DTARGET_DEVICE="\"$(TARGET_DEVICE)\""

ifneq ($(MR_DEVICE_HOOKS),)
ifeq ($(MR_DEVICE_HOOKS_VER),)
    $(info MR_DEVICE_HOOKS is set but MR_DEVICE_HOOKS_VER is not specified!)
else
    LOCAL_CFLAGS += -DMR_DEVICE_HOOKS=$(MR_DEVICE_HOOKS_VER)
    LOCAL_SRC_FILES += ../../../../$(MR_DEVICE_HOOKS)
endif
endif

ifeq ($(MR_ENCRYPTION),true)
    LOCAL_CFLAGS += -DMR_ENCRYPTION
    LOCAL_SRC_FILES += encryption.c
endif

ifeq ($(MR_ENCRYPTION_FAKE_PROPERTIES),true)
    LOCAL_CFLAGS += -DMR_ENCRYPTION_FAKE_PROPERTIES
endif

ifeq ($(MR_USE_DEBUGFS_MOUNT),true)
    LOCAL_CFLAGS += -DMR_USE_DEBUGFS_MOUNT
endif

include $(BUILD_EXECUTABLE)
