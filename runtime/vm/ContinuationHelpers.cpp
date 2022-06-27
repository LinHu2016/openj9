/*******************************************************************************
 * Copyright (c) 2022, 2022 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/
#include "j9.h"
#include "j9comp.h"
#include "j9protos.h"
#include "j9vmnls.h"
#include "ut_j9vm.h"
#include "vm_api.h"
#include "OutOfLineINL.hpp"
#include "ContinuationHelpers.hpp"

extern "C" {
#if JAVA_SPEC_VERSION >= 19

J9_DECLARE_CONSTANT_UTF8(continuationClass_name, "jdk/internal/vm/Continuation");
J9_DECLARE_CONSTANT_UTF8(execute_sig, "(Ljdk/internal/vm/Continuation;)V");
J9_DECLARE_CONSTANT_UTF8(execute_name, "execute");

extern void c_cInterpreter(J9VMThread *currentThread);

BOOLEAN
createContinuation(J9VMThread *currentThread, j9object_t continuationObject)
{
	J9JavaVM *vm = currentThread->javaVM;
	PORT_ACCESS_FROM_PORT(vm->portLibrary);
	BOOLEAN result = TRUE;
	J9JavaStack *stack = NULL;
	J9SFJNINativeMethodFrame *frame = NULL;

	J9VMContinuation *continuation = (J9VMContinuation *)j9mem_allocate_memory(sizeof(J9VMContinuation), OMRMEM_CATEGORY_THREADS);
	if (NULL == continuation) {
		vm->internalVMFunctions->setNativeOutOfMemoryError(currentThread, 0, 0);
		result = FALSE;
		goto end;
	}

	memset(continuation, 0, sizeof(J9VMContinuation));

#ifdef J9VM_INTERP_GROWABLE_STACKS
#define VMTHR_INITIAL_STACK_SIZE ((vm->initialStackSize > (UDATA) vm->stackSize) ? vm->stackSize : vm->initialStackSize)
#else
#define VMTHR_INITIAL_STACK_SIZE vm->stackSize
#endif

	if ((stack = allocateJavaStack(vm, VMTHR_INITIAL_STACK_SIZE, NULL)) == NULL) {
		vm->internalVMFunctions->setNativeOutOfMemoryError(currentThread, 0, 0);
		result = FALSE;
		goto end;
	}

#undef VMTHR_INITIAL_STACK_SIZE

	continuation->stackObject = stack;
	continuation->stackOverflowMark = continuation->stackOverflowMark2 = J9JAVASTACK_STACKOVERFLOWMARK(stack);

	frame = ((J9SFJNINativeMethodFrame*)stack->end) - 1;
	frame->method = NULL;
	frame->specialFrameFlags = 0;
	frame->savedCP = NULL;
	frame->savedPC = (U_8*)(UDATA)J9SF_FRAME_TYPE_END_OF_STACK;
	frame->savedA0 = (UDATA*)(UDATA)J9SF_A0_INVISIBLE_TAG;
	continuation->sp = (UDATA*)frame;
	continuation->literals = (J9Method*)0;
	continuation->pc = (U_8*)J9SF_FRAME_TYPE_JNI_NATIVE_METHOD;
	continuation->arg0EA = (UDATA*)&frame->savedA0;
	continuation->stackObject->isVirtual = TRUE;

	J9VMJDKINTERNALVMCONTINUATION_SET_VMREF(currentThread, continuationObject, continuation);

	/* GC Hook to register Continuation object */
end:
	return result;
}

void JNICALL
resumeContinuation(J9VMThread *currentThread, J9VMContinuation *continuation)
{
	J9VMEntryLocalStorage newELS;
	newELS = continuation->entryLocalStorage;
	newELS.oldEntryLocalStorage = currentThread->entryLocalStorage;;
	currentThread->entryLocalStorage = &newELS;

	if (NULL != newELS.oldEntryLocalStorage) {
		/* Assuming oldELS > newELS, bytes used is (oldELS - newELS) */
		UDATA freeBytes = currentThread->currentOSStackFree;
		UDATA usedBytes = ((UDATA)newELS.oldEntryLocalStorage - (UDATA)&newELS);
		freeBytes -= usedBytes;
		currentThread->currentOSStackFree = freeBytes;

		if ((IDATA)freeBytes < J9_OS_STACK_GUARD) {
			if (J9_ARE_NO_BITS_SET(currentThread->privateFlags, J9_PRIVATE_FLAGS_CONSTRUCTING_EXCEPTION)) {
				setCurrentExceptionNLS(currentThread, J9VMCONSTANTPOOL_JAVALANGSTACKOVERFLOWERROR, J9NLS_VM_OS_STACK_OVERFLOW);
				currentThread->currentOSStackFree += usedBytes;
			}
		}
	}

	VM_OutOfLineINL_Helpers::restoreInternalNativeStackFrame(currentThread);
	VM_OutOfLineINL_Helpers::returnSingle(currentThread, JNI_TRUE, 1);

	currentThread->returnValue = J9_BCLOOP_EXECUTE_BYTECODE;

	c_cInterpreter(currentThread);

	restoreCallInFrame(currentThread);
}

BOOLEAN
enterContinuation(J9VMThread *currentThread, j9object_t continuationObject)
{
	BOOLEAN result = TRUE;
	jboolean started = J9VMJDKINTERNALVMCONTINUATION_STARTED(currentThread, continuationObject);
	J9VMContinuation *continuation = J9VMJDKINTERNALVMCONTINUATION_VMREF(currentThread, continuationObject);

	Assert_VM_Null(currentThread->currentContinuation);

	VM_ContinuationHelpers::swapFieldsWithContinuation(currentThread, continuation);
	currentThread->currentContinuation = continuation;

	Assert_VM_notNull(currentThread->currentContinuation);

	if (started) {
		/* resuming Continuation from yield */
		resumeContinuation(currentThread, continuation);
	} else {
		/* start new */
		UDATA args[] = { (UDATA) continuationObject };
		J9NameAndSignature executeNameAndSig = { (J9UTF8*)&execute_name, (J9UTF8*)&execute_sig };

		J9VMJDKINTERNALVMCONTINUATION_SET_STARTED(currentThread, continuationObject, JNI_TRUE);

		runStaticMethod(currentThread, J9UTF8_DATA(&continuationClass_name), &executeNameAndSig, 1, args);
	}

	J9VMJDKINTERNALVMCONTINUATION_SET_FINISHED(currentThread, continuationObject, JNI_TRUE);

	currentThread->currentContinuation = NULL;

	VM_ContinuationHelpers::swapFieldsWithContinuation(currentThread, continuation);

	/* For some reason the JCL swap thread objects when the VirtualThread dies, but it does
	 * on enter and yield.
	 */
	currentThread->threadObject = currentThread->carrierThreadObject;

	Assert_VM_Null(currentThread->currentContinuation);

	return result;
}

BOOLEAN
yieldContinuation(J9VMThread *currentThread, j9object_t scope)
{
	BOOLEAN result = TRUE;
	J9VMContinuation *continuation = currentThread->currentContinuation;

	/* need to check pin state before yielding */

	VM_ContinuationHelpers::swapFieldsWithContinuation(currentThread, continuation);
	/* pop the current ELS struct from the J9VMThread and store its info in J9VMContinuation struct */
	VM_ContinuationHelpers::popAndStoreELS(currentThread, continuation);

	/* Swap to parent Continuation for nested Continuation */
	/*
	j9object_t parentContinuation = J9VMJDKINTERNALVMCONTINUATION_PARENT(currentThread, continuationObject);
	if (NULL != parentContinuation) {
		currentThread->currentContinuation = (J9VMContinuation *)(UDATA)J9VMJDKINTERNALVMCONTINUATION_VMREF(currentThread, parentContinuation);
	} else {
		currentThread->currentContinuation = NULL;
	}
	*/

	currentThread->currentContinuation = NULL;

	return result;
}

#endif /* JAVA_SPEC_VERSION >= 19 */
} /* extern "C" */
 /* extern "C" */
