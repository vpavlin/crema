LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := logosdelivery
LOCAL_SRC_FILES := ../$(TARGET_ARCH_ABI)/liblogosdelivery.so

include $(PREBUILT_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := logos_messaging_ffi.c
LOCAL_MODULE := logos_messaging_jni
LOCAL_LDLIBS := -llog
LOCAL_SHARED_LIBRARIES := logosdelivery

include $(BUILD_SHARED_LIBRARY)