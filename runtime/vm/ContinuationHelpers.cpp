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
#include "ContinuationHelpers.hpp"
#include "OutOfLineINL.hpp"


extern "C" {

J9_DECLARE_CONSTANT_UTF8(continuationClass_name, "jdk/internal/vm/Continuation");
J9_DECLARE_CONSTANT_UTF8(execute_sig, "(Ljdk/internal/vm/Continuation;)V");
J9_DECLARE_CONSTANT_UTF8(execute_name, "execute");

extern void c_cInterpreter(J9VMThread *currentThread);

BOOLEAN
createContinuation(J9VMThread *currentThread, j9object_t continuationObject)
{
	J9JavaVM *vm = currentThread->javaVM;
	PORT_ACCESS_FROM_VMC(currentThread);
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
	continuation->stackOverflowMark2 = J9JAVASTACK_STACKOVERFLOWMARK(stack);
	continuation->stackOverflowMark = continuation->stackOverflowMark2;

	frame = ((J9SFJNINativeMethodFrame*)stack->end) - 1;
	frame->method = NULL;
	frame->specialFrameFlags = 0;
	frame->savedCP = NULL;
	frame->savedPC = (U_8*)(UDATA)J9SF_FRAME_TYPE_END_OF_STACK;
	frame->savedA0 = (UDATA*)(UDATA)J9SF_A0_INVISIBLE_TAG;
	continuation->sp = (UDATA*)frame;
	continuation->literals = NULL;
	continuation->pc = (U_8*)J9SF_FRAME_TYPE_JNI_NATIVE_METHOD;
	continuation->arg0EA = (UDATA*)&frame->savedA0;
	continuation->stackObject->isVirtual = TRUE;

	J9VMJDKINTERNALVMCONTINUATION_SET_VMREF(currentThread, continuationObject, continuation);

	/* GC Hook to register Continuation object */
end:
	return result;
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

	/* Reset counters which determine if the current continuation is pinned. */
	currentThread->continuationPinCount = 0;
	currentThread->ownedMonitorCount = 0;
	currentThread->callOutCount = 0;

	if (started) {
		/* resuming Continuation from yield */
		VM_OutOfLineINL_Helpers::restoreInternalNativeStackFrame(currentThread);
		VM_OutOfLineINL_Helpers::returnSingle(currentThread, JNI_TRUE, 1);
		result = FALSE;
	} else {
		/* start new Continuation execution */
		J9VMJDKINTERNALVMCONTINUATION_SET_STARTED(currentThread, continuationObject, JNI_TRUE);
		/* prepare callin frame, send method will be set by interpreter */
		J9SFJNICallInFrame *frame = ((J9SFJNICallInFrame*)currentThread->sp) - 1;
		UDATA flags = J9_SSF_REL_VM_ACC;
#if defined(J9VM_OPT_JAVA_OFFLOAD_SUPPORT)
		J9JavaVM *vm = currentThread->javaVM;
		if (NULL != vm->javaOffloadSwitchOnWithReasonFunc) {
			bool isOffloadWithSubtasks = J9_ARE_ANY_BITS_SET(currentThread->javaOffloadState, J9_JNI_OFFLOAD_WITH_SUBTASKS_FLAG);
			if (isOffloadWithSubtasks) {
				/* keep offload enabled but turn off allowing created subtasks to offload */
				vm->javaOffloadSwitchOnDisableSubtasksWithReasonFunc(currentThread, J9_JNI_OFFLOAD_SWITCH_INTERPRETER);
				currentThread->javaOffloadState &= ~J9_JNI_OFFLOAD_WITH_SUBTASKS_FLAG;
				flags |= J9_SSF_JNI_OFFLOAD_WAS_WITH_SUBTASKS;
			} else if (0 == currentThread->javaOffloadState) {
				vm->javaOffloadSwitchOnWithReasonFunc(currentThread, J9_JNI_OFFLOAD_SWITCH_INTERPRETER);
			}
			Assert_VM_unequal(currentThread->javaOffloadState & J9_JNI_OFFLOAD_MAX_VALUE, J9_JNI_OFFLOAD_MAX_VALUE);
			currentThread->javaOffloadState += 1;
		}
#endif /* J9VM_OPT_JAVA_OFFLOAD_SUPPORT */

		frame->exitAddress = NULL;
		frame->specialFrameFlags = flags;
		frame->savedCP = currentThread->literals;
		frame->savedPC = currentThread->pc;
		frame->savedA0 = (UDATA*)((UDATA)currentThread->arg0EA | J9SF_A0_INVISIBLE_TAG);
		currentThread->sp = (UDATA*)frame;
		currentThread->pc = currentThread->javaVM->callInReturnPC;
		currentThread->literals = 0;
		currentThread->arg0EA = (UDATA*)&frame->savedA0;

		/* push argument to stack */
		*--currentThread->sp = (UDATA)continuationObject;
	}

	return result;
}

BOOLEAN
yieldContinuation(J9VMThread *currentThread)
{
	BOOLEAN result = TRUE;
	J9VMContinuation *continuation = currentThread->currentContinuation;

	Assert_VM_notNull(currentThread->currentContinuation);

	VM_ContinuationHelpers::swapFieldsWithContinuation(currentThread, continuation);

	currentThread->currentContinuation = NULL;

	return result;
}

jint
isPinnedContinuation(J9VMThread *currentThread)
{
	jint result = 0;

	if (currentThread->continuationPinCount > 0) {
		result = J9VM_CONTINUATION_PINNED_REASON_CRITICAL_SECTION;
	} else if (currentThread->ownedMonitorCount > 0) {
		result = J9VM_CONTINUATION_PINNED_REASON_MONITOR;
	} else if (currentThread->callOutCount > 0) {
		/* TODO: This check should be changed from > 1 to > 0 once the call-ins are no
		 * longer used and the new design for single cInterpreter is implemented.
		 */
		result = J9VM_CONTINUATION_PINNED_REASON_NATIVE;
	} else {
		/* Do nothing. */
	}

	return result;
}

} /* extern "C" */
