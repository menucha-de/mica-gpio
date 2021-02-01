#include <havis_device_io_common_ext_NativeHardwareManager.h>

#include <jni.h>
#include <linux/jni_md.h>
#include <stdlib.h>
#include <string.h>

#include "../include/mica_gpio.h"

struct runtime {
	JavaVM *jvm;
	JNIEnv *env;
	jobject listener;
	jclass state;
	jclass state_event;
	jclass state_listener;
};
typedef struct runtime runtime;

/**
 * Gets Lhavis/device/io/State; object from enumeration
 * @returns Java Lhavis/device/io/State; object
 */
jobject get_state(JNIEnv *env, jclass clazz, enum MICA_GPIO_STATE state) {
	if (clazz != NULL) {
		const char *name = NULL;
		switch (state) {
		case LOW:
			name = "LOW";
			break;
		case HIGH:
			name = "HIGH";
			break;
		}
		if (name != NULL) {
			jfieldID field = (*env)->GetStaticFieldID(env, clazz, name, "Lhavis/device/io/State;");
			if (field != NULL) {
				jobject state = (*env)->GetStaticObjectField(env, clazz, field);
				if (state != NULL)
					return state;
			}
		}
	}
	return NULL;
}

const char *get_value(JNIEnv *env, jclass clazz, jobject param) {
	if (clazz != NULL) {
		jmethodID method = (*env)->GetMethodID(env, clazz, "name", "()Ljava/lang/String;");
		if (method != NULL) {
			jstring value = (jstring) (*env)->CallObjectMethod(env, param, method);
			if (value != NULL) {
				return (*env)->GetStringUTFChars(env, value, 0);
			}
		}
	}
	return NULL;
}

/*
 * Class:     havis_device_io_common_ext_NativeHardwareManager
 * Method:    getState
 * Signature: (S)Lhavis/device/io/State;
 */
JNIEXPORT jobject JNICALL Java_havis_device_io_common_ext_NativeHardwareManager_getState(JNIEnv *env, jobject this, jshort id) {
	return get_state(env, (*env)->FindClass(env, "havis/device/io/State"), mica_gpio_get_state(id));
}

/*
 * Class:     havis_device_io_common_ext_NativeHardwareManager
 * Method:    setState
 * Signature: (SLhavis/device/io/State;)V
 */
JNIEXPORT void JNICALL Java_havis_device_io_common_ext_NativeHardwareManager_setState(JNIEnv *env, jobject this, jshort id, jobject state) {
	if (state != NULL) {
		jclass clazz = (*env)->FindClass(env, "havis/device/io/State");
		const char *value = get_value(env, clazz, state);
		if (strcmp(value, "HIGH") == 0)
			mica_gpio_set_state(id, HIGH);
		else if (strcmp(value, "LOW") == 0)
			mica_gpio_set_state(id, LOW);
		(*env)->ReleaseStringUTFChars(env, state, value);
	}
}

/*
 * Class:     havis_device_io_common_ext_NativeHardwareManager
 * Method:    getDirection
 * Signature: (S)Lhavis/device/io/Direction;
 */
JNIEXPORT jobject JNICALL Java_havis_device_io_common_ext_NativeHardwareManager_getDirection(JNIEnv *env, jobject this, jshort id) {
	enum MICA_GPIO_DIRECTION direction = mica_gpio_get_direction(id);
	if (direction > -1) {
		jclass clazz = (*env)->FindClass(env, "havis/device/io/Direction");
		if (clazz != NULL) {
			const char *name = NULL;
			switch (direction) {
			case INPUT:
				name = "INPUT";
				break;
			case OUTPUT:
				name = "OUTPUT";
				break;
			}
			if (name != NULL) {
				jfieldID field = (*env)->GetStaticFieldID(env, clazz, name, "Lhavis/device/io/Direction;");
				if (field != NULL) {
					jobject direction = (*env)->GetStaticObjectField(env, clazz, field);
					if (direction != NULL)
						return direction;
				}
			}
		}
	}
	return NULL;
}

/*
 * Class:     havis_device_io_common_ext_NativeHardwareManager
 * Method:    setDirection
 * Signature: (SLhavis/device/io/Direction;)V
 */
JNIEXPORT void JNICALL Java_havis_device_io_common_ext_NativeHardwareManager_setDirection(JNIEnv *env, jobject this, jshort id, jobject direction) {
	if (direction != NULL) {
		jclass clazz = (*env)->FindClass(env, "havis/device/io/Direction");
		const char* value = get_value(env, clazz, direction);
		if (strcmp(value, "INPUT") == 0) {
			mica_gpio_set_direction(id, INPUT);
		} else if (strcmp(value, "OUTPUT") == 0) {
			mica_gpio_set_direction(id, OUTPUT);
		}
		(*env)->ReleaseStringUTFChars(env, direction, value);
	}
}

/*
 * Class:     havis_device_io_common_ext_NativeHardwareManager
 * Method:    getEnable
 * Signature: (S)Z
 */
JNIEXPORT jboolean JNICALL Java_havis_device_io_common_ext_NativeHardwareManager_getEnable(JNIEnv *env, jobject this, jshort id) {
	return mica_gpio_get_enable(id);
}

/*
 * Class:     havis_device_io_common_ext_NativeHardwareManager
 * Method:    setEnable
 * Signature: (SZ)V
 */
JNIEXPORT void JNICALL Java_havis_device_io_common_ext_NativeHardwareManager_setEnable(JNIEnv *env, jobject this, jshort id, jboolean enable) {
	mica_gpio_set_enable(id, enable);
}

/*
 * Class:     havis_device_io_common_ext_NativeHardwareManager
 * Method:    getCount
 * Signature: ()S
 */
JNIEXPORT jshort JNICALL Java_havis_device_io_common_ext_NativeHardwareManager_getCount(JNIEnv *env, jobject this) {
	return MICA_GPIO_SIZE;
}

/**
 * Runs listener thread. Calls listener if state changed
 */
void call(int id, enum MICA_GPIO_STATE state, void *data) {
	struct runtime *rt = data;
	JNIEnv *env;
	switch (id) {
	case 0:
		(*rt->jvm)->AttachCurrentThread(rt->jvm, (void**) &(rt->env), NULL);
		break;
	case -1:
		(*rt->jvm)->DetachCurrentThread(rt->jvm);
		break;
	default:
		env = rt->env;
		// create state event
		jmethodID init = (*env)->GetMethodID(env, rt->state_event, "<init>", "(SLhavis/device/io/State;)V");
		jobject event = (*env)->NewObject(env, rt->state_event, init, id, get_state(env, rt->state, state));

		// call stateChanged
		jmethodID state_changed = (*env)->GetMethodID(env, rt->state_listener, "stateChanged", "(Lhavis/device/io/StateEvent;)V");
		(*env)->CallVoidMethod(env, rt->listener, state_changed, event);
		break;
	}
}

/*
 * Class:     havis_device_io_common_ext_NativeHardwareManager
 * Method:    setListener
 * Signature: (Lhavis/device/io/StateListener;)V
 */
JNIEXPORT void JNICALL Java_havis_device_io_common_ext_NativeHardwareManager_setListener(JNIEnv *env, jobject this, jobject listener) {
	runtime *rt = NULL;
	if (listener) {
		rt = malloc(sizeof(runtime));
		(*env)->GetJavaVM(env, &(rt->jvm));
		rt->listener = (*env)->NewGlobalRef(env, listener);
		rt->state = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "havis/device/io/State"));
		rt->state_event = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "havis/device/io/StateEvent"));
		rt->state_listener = (*env)->NewGlobalRef(env, (*env)->FindClass(env, "havis/device/io/StateListener"));
	}
	rt = mica_gpio_set_callback(listener ? call : NULL, rt);
	if (rt) {
		(*env)->GetJavaVM(env, &(rt->jvm));
		(*env)->DeleteGlobalRef(env, rt->state_listener);
		(*env)->DeleteGlobalRef(env, rt->state_event);
		(*env)->DeleteGlobalRef(env, rt->state);
		(*env)->DeleteGlobalRef(env, rt->listener);
		free(rt);
	}
}
