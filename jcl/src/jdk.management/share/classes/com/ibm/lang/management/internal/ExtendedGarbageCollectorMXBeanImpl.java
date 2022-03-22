/*[INCLUDE-IF JAVA_SPEC_VERSION >= 8]*/
/*******************************************************************************
 * Copyright (c) 2016, 2022 IBM Corp. and others
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
package com.ibm.lang.management.internal;

import javax.management.MBeanNotificationInfo;

import com.ibm.java.lang.management.internal.GarbageCollectorMXBeanImpl;
import com.ibm.lang.management.GarbageCollectorMXBean;
import com.sun.management.GarbageCollectionNotificationInfo;
import com.sun.management.GcInfo;
import com.sun.management.internal.GcInfoUtil;

import java.lang.management.MemoryType;
import java.lang.management.MemoryUsage;
import java.lang.management.MemoryPoolMXBean;
import java.lang.management.ManagementFactory;
import java.util.HashMap;
import java.util.Map;
import java.util.List;

/**
 * Runtime type for {@link com.ibm.lang.management.GarbageCollectorMXBean}.
 */
public final class ExtendedGarbageCollectorMXBeanImpl
		extends GarbageCollectorMXBeanImpl
		implements GarbageCollectorMXBean {
	
	private static String[] poolNames = null;

	ExtendedGarbageCollectorMXBeanImpl(String domainName, String name, int id, ExtendedMemoryMXBeanImpl memBean) {
		super(domainName, name, id, memBean);
	}

	/**
	 * {@inheritDoc}
	 */
	@Override
	public MBeanNotificationInfo[] getNotificationInfo() {
		// We know what kinds of notifications we can emit whereas the
		// notifier delegate does not. So, for this method, no delegating.
		// Instead respond using our own metadata.
		MBeanNotificationInfo info = new MBeanNotificationInfo(
				new String[] { GarbageCollectionNotificationInfo.GARBAGE_COLLECTION_NOTIFICATION },
				javax.management.Notification.class.getName(),
				"GarbageCollection Notification"); //$NON-NLS-1$
		return new MBeanNotificationInfo[] { info };
	}

	/**
	 * {@inheritDoc}
	 */
	@Override
	public GcInfo getLastGcInfo() {
		return this.getLastGcInfoImpl(id);
	}

	/**
	 * @return the gc information of the most recent collection
	 * @see #getLastGcInfo()
	 */
	private native GcInfo getLastGcInfoImpl(int id);

	static GcInfo buildGcInfo(long index, long startTime, long endTime,
							long[] initialSize, long[] preUsed, long[] preCommitted, long[] preMax,
							long[] postUsed, long[] postCommitted, long[] postMax) {
		/* retrieve the names of MemoryPools*/
		if (null == poolNames) {
			List<MemoryPoolMXBean> memoryPoolList = ExtendedMemoryMXBeanImpl.getInstance().getMemoryPoolMXBeans(false);
			poolNames = new String[memoryPoolList.size()];
			int idx = 0;
			int previousID = 0;
			int id = 0;
			boolean nonHeap = false;
			
			for (MemoryPoolMXBean bean : memoryPoolList) {
				poolNames[idx++] = bean.getName();
				id = ((MemoryPoolMXBeanImpl) bean).getID();
				System.out.println("MemoryPoolMXBean order: ID=" + id +", Name=" + bean.getName());
				
				if ((bean.getType() == MemoryType.NON_HEAP) && !nonHeap ) {
					nonHeap = true;
				} else {
					if ((id < previousID) || ((bean.getType() != MemoryType.NON_HEAP) && (nonHeap)) ) {
						System.out.println("MemoryPoolMXBean order error: currentID=" + id +", previousID=" + previousID);
					}
				}
				previousID = id;
			}
		}		
		
		int poolNamesLength = poolNames.length;
		/*[IF JAVA_SPEC_VERSION >= 19]
		Map<String, MemoryUsage> usageBeforeGc = HashMap.newHashMap(poolNamesLength);
		Map<String, MemoryUsage> usageAfterGc = HashMap.newHashMap(poolNamesLength);
		/*[ELSE] JAVA_SPEC_VERSION >= 19 */
		// HashMap.DEFAULT_LOAD_FACTOR is 0.75
		Map<String, MemoryUsage> usageBeforeGc = new HashMap<>(poolNamesLength * 4 / 3);
		Map<String, MemoryUsage> usageAfterGc = new HashMap<>(poolNamesLength * 4 / 3);
		/*[ENDIF] JAVA_SPEC_VERSION >= 19 */
		boolean bError = false;
		for (int count = 0; count < poolNames.length; ++count) {
			
			/* debugging the potential issues
		    the value of init or max is negative but not -1; or
		    the value of used or committed is negative; or
		    used is greater than the value of committed; or
		    committed is greater than the value of max max if defined.
			*/
			
			if ((preUsed[count] > preCommitted[count]) || 
				((preMax[count] != -1) && (preCommitted[count] > preMax[count])) ||
				(preUsed[count] < 0) ||
				(preCommitted[count] < 0) ||
				(initialSize[count] < -1) ||
				(preMax[count] < -1)) {
				bError = true;
				if (preCommitted[count] < 0) {
					System.out.println("preCommitted size=" + preCommitted[count] + " < 0"); 
				}
				System.out.println("MemoryUsage BeforeGc Error! index=" + index + ", count=" + count + ", poolName: " + poolNames[count] +", init=" + initialSize[count] + ", preUsed=" + preUsed[count] + ", preCommitted=" + preCommitted[count] + ", preMax=" + preMax[count]);
			}
			
			if ((postUsed[count] > postCommitted[count]) || 
					((postMax[count] != -1) && (postCommitted[count] > postMax[count])) ||
					(postUsed[count] < 0) ||
					(postCommitted[count] < 0) ||
					(initialSize[count] < -1) ||
					(postMax[count] < -1)) {
				bError = true;
				if (postCommitted[count] < 0) {
					System.out.println("postCommitted size=" + postCommitted[count] + " < 0"); 
				}
				System.out.println("MemoryUsage AfterGc Error! index=" + index + ", count=" + count + ", poolName: " + poolNames[count] +", init=" + initialSize[count] + ", postUsed=" + postUsed[count] + ", postCommitted=" + postCommitted[count] + ", postMax=" + postMax[count]);
			}
			
			if (bError) {
				printMemoryUsages(poolNames, initialSize, preUsed, preCommitted, preMax, postUsed, postCommitted, postMax);
				bError = false;
			}
			
			usageBeforeGc.put(poolNames[count], new MemoryUsage(initialSize[count], preUsed[count], preCommitted[count], preMax[count]));
			usageAfterGc.put(poolNames[count], new MemoryUsage(initialSize[count], postUsed[count], postCommitted[count], postMax[count]));
		}
		return GcInfoUtil.newGcInfoInstance(index, startTime, endTime, usageBeforeGc, usageAfterGc);
	}
	
	static void printMemoryUsages(String[] poolNames, long[] initialSize, long[] preUsed, long[] preCommitted, long[] preMax,
									long[] postUsed, long[] postCommitted, long[] postMax) {
		for (int count = 0; count < poolNames.length; ++count) {
			System.out.println("MemoryUsage" + count + ": poolName=" + poolNames[count] + ", init=" + initialSize[count] + ", preUsed=" + preUsed[count] + ", preCommitted=" + preCommitted[count] + ", preMax=" + preMax[count] + ", postUsed=" + postUsed[count] + ", postCommitted=" + postCommitted[count] + ", postMax=" + postMax[count]);
		}
	}

}
