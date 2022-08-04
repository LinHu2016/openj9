
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

#include "HeapWalkerDelegate.hpp"

#if JAVA_SPEC_VERSION >= 19
#include "j9.h"
#include "ObjectModel.hpp"
#include "VMHelpers.hpp"
#include "VMThreadStackSlotIterator.hpp"
#include "HeapWalker.hpp"
#endif /* JAVA_SPEC_VERSION >= 19 */


void
MM_HeapWalkerDelegate::doObject(MM_EnvironmentBase *env, omrobjectptr_t objectPtr, MM_HeapWalkerSlotFunc function, void *userData)
{
#if JAVA_SPEC_VERSION >= 19
	switch(_objectModel->getScanType(objectPtr)) {
	case GC_ObjectModel::SCAN_CONTINUATION_OBJECT:
		doContinuationObject(env, objectPtr, function, userData);
		break;
	default:
		break;
	}
#endif /* JAVA_SPEC_VERSION >= 19 */
}

#if JAVA_SPEC_VERSION >= 19
/**
 * @todo Provide function documentation
 */
void
stackSlotIterator4HeapWalker(J9JavaVM *javaVM, J9Object **slotPtr, void *localData, J9StackWalkState *walkState, const void *stackLocation)
{
	StackIteratorData4HeapWalker *data = (StackIteratorData4HeapWalker *)localData;
	data->heapWalker->heapWalkerSlotCallback(data->env, slotPtr, data->function, data->userData);
}

void
MM_HeapWalkerDelegate::doContinuationObject(MM_EnvironmentBase *env, omrobjectptr_t objectPtr, MM_HeapWalkerSlotFunc function, void *userData)
{
	J9VMThread *currentThread = (J9VMThread *)env->getLanguageVMThread();
	jboolean started = J9VMJDKINTERNALVMCONTINUATION_STARTED(currentThread, objectPtr);
	J9VMContinuation *j9vmContinuation = J9VMJDKINTERNALVMCONTINUATION_VMREF(currentThread, objectPtr);
	if (started && (NULL != j9vmContinuation)) {
		J9VMThread continuationThread;
		J9VMEntryLocalStorage newELS;
		memset(&continuationThread, 0, sizeof(J9VMThread));
		continuationThread.entryLocalStorage = &newELS;
		continuationThread.javaVM = currentThread->javaVM;
		VM_VMHelpers::copyJavaStacksFromJ9VMContinuation(&continuationThread, j9vmContinuation);

		StackIteratorData4HeapWalker localData;
		localData.heapWalker = _heapWalker;
		localData.env = env;
		localData.fromObject = objectPtr;
		localData.function = function;
		localData.userData = userData;


		bool bStackFrameClassWalkNeeded = false;
//#if defined(J9VM_GC_DYNAMIC_CLASS_UNLOADING)
//		bStackFrameClassWalkNeeded = isDynamicClassUnloadingEnabled();
//#endif /* J9VM_GC_DYNAMIC_CLASS_UNLOADING */

		GC_VMThreadStackSlotIterator::scanSlots(currentThread, &continuationThread, (void *)&localData, stackSlotIterator4HeapWalker, bStackFrameClassWalkNeeded, false);
		/*debug*/
		PORT_ACCESS_FROM_ENVIRONMENT(env);
		j9tty_printf(PORTLIB, "MM_HeapWalkerDelegate::doContinuationObject GC_VMThreadStackSlotIterator::scanSlots env=%p\n",env);
	}
}
#endif /* JAVA_SPEC_VERSION >= 19 */
