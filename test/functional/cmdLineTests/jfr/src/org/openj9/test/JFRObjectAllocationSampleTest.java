/*
 * Copyright IBM Corp. and others 2026
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
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 */
package org.openj9.test;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.List;

import jdk.jfr.FlightRecorder;
import jdk.jfr.Recording;
import jdk.jfr.RecordingState;
import jdk.jfr.consumer.RecordedEvent;
import jdk.jfr.consumer.RecordingFile;

/**
 * Verifies that the enabled and throttle settings of jdk.ObjectAllocationSample
 * are honoured by the VM.
 *
 * Usage:
 *   dynamic         Change the settings of a running recording and check the number of
 *                   events produced in each phase.
 *   cmdline none    The recording was started with -XX:StartFlightRecording and the event
 *                   is expected to be disabled (enabled=false or throttle=0/s): no events.
 *   cmdline some    The recording was started with -XX:StartFlightRecording and the event
 *                   is expected to produce events (e.g. throttle=off or throttle=10/s).
 */
public class JFRObjectAllocationSampleTest {

	private static final String EVENT_NAME = "jdk.ObjectAllocationSample";

	/* Total bytes allocated per phase and the size of each allocation. */
	private static final int ALLOCATION_SIZE = 1024;
	private static final int ALLOCATION_COUNT = 64 * 1024;

	/* With throttle=off every allocation is sampled, so at least this many events are expected. */
	private static final int MIN_UNTHROTTLED_EVENTS = 100;

	private static final long SETTLE_MILLIS = 500;

	/* Keep allocations reachable so that they cannot be optimized away. */
	private static final Object[] sink = new Object[64];

	private static boolean failed = false;

	public static void main(String[] args) throws Throwable {
		String mode = (args.length > 0) ? args[0] : "dynamic";

		if ("dynamic".equals(mode)) {
			runDynamic();
		} else if ("cmdline".equals(mode) && (args.length > 1)) {
			runCmdLine(args[1]);
		} else {
			System.out.println("Unknown arguments");
			failed = true;
		}

		System.out.println(failed ? "TEST FAILED" : "TEST PASSED");
	}

	private static void allocate() {
		for (int i = 0; i < ALLOCATION_COUNT; i++) {
			sink[i % sink.length] = new byte[ALLOCATION_SIZE];
		}
	}

	private static void check(String description, boolean condition) {
		System.out.println((condition ? "OK: " : "ERROR: ") + description);
		if (!condition) {
			failed = true;
		}
	}

	private static void configure(Recording recording, boolean enabled, String throttle) throws InterruptedException {
		if (enabled) {
			recording.enable(EVENT_NAME).with("throttle", throttle);
		} else {
			recording.disable(EVENT_NAME);
		}
		System.out.println("Settings: enabled=" + enabled + ", throttle=" + throttle);
		Thread.sleep(SETTLE_MILLIS);
	}

	/**
	 * Allocate with the current settings and return the time just after the allocation
	 * and a grace period, which separates this phase from the next one.
	 */
	private static Instant runPhase() throws InterruptedException {
		allocate();
		Thread.sleep(SETTLE_MILLIS);
		return Instant.now();
	}

	private static void runDynamic() throws Throwable {
		Path dumpFile = Files.createTempFile("jfrObjectAllocationSample", ".jfr");
		try (Recording recording = new Recording()) {
			recording.enable(EVENT_NAME);
			recording.start();

			/* Phase 0: event disabled */
			configure(recording, false, "off");
			Instant end0 = runPhase();

			/* Phase 1: unthrottled */
			configure(recording, true, "off");
			Instant end1 = runPhase();

			/* Phase 2: throttle 0/s disables sampling */
			configure(recording, true, "0/s");
			Instant end2 = runPhase();

			/* Phase 3: a low rate; sampling is re-enabled after being switched off */
			configure(recording, true, "10/s");
			Instant end3 = runPhase();

			/* Phase 4: event disabled again */
			configure(recording, false, "off");
			Instant end4 = runPhase();

			recording.stop();
			recording.dump(dumpFile);

			int[] counts = new int[5];
			Instant[] ends = {end0, end1, end2, end3, end4};
			for (RecordedEvent event : RecordingFile.readAllEvents(dumpFile)) {
				if (EVENT_NAME.equals(event.getEventType().getName())) {
					int phase = 0;
					while ((phase < (ends.length - 1)) && event.getStartTime().isAfter(ends[phase])) {
						phase += 1;
					}
					counts[phase] += 1;
				}
			}
			for (int i = 0; i < counts.length; i++) {
				System.out.println("Phase " + i + ": " + counts[i] + " " + EVENT_NAME + " events");
			}

			check("phase 0 (disabled) has no events", 0 == counts[0]);
			check("phase 1 (throttle=off) has at least " + MIN_UNTHROTTLED_EVENTS + " events",
					MIN_UNTHROTTLED_EVENTS <= counts[1]);
			check("phase 2 (throttle=0/s) has no events", 0 == counts[2]);
			check("phase 3 (throttle=10/s) has events", 0 < counts[3]);
			check("phase 3 (throttle=10/s) has fewer events than phase 1 (throttle=off)", counts[3] < counts[1]);
			check("phase 4 (disabled) has no events", 0 == counts[4]);
		} finally {
			Files.deleteIfExists(dumpFile);
		}
	}

	private static void runCmdLine(String expectation) throws IOException {
		boolean expectEvents;
		if ("some".equals(expectation)) {
			expectEvents = true;
		} else if ("none".equals(expectation)) {
			expectEvents = false;
		} else {
			System.out.println("Unknown expectation: " + expectation);
			failed = true;
			return;
		}

		allocate();

		Recording recording = null;
		for (Recording candidate : FlightRecorder.getFlightRecorder().getRecordings()) {
			if (RecordingState.RUNNING == candidate.getState()) {
				recording = candidate;
				break;
			}
		}
		if (null == recording) {
			System.out.println("ERROR: no running recording found");
			failed = true;
			return;
		}

		Path dumpFile = Files.createTempFile("jfrObjectAllocationSample", ".jfr");
		try {
			recording.dump(dumpFile);
			int count = 0;
			List<RecordedEvent> events = RecordingFile.readAllEvents(dumpFile);
			for (RecordedEvent event : events) {
				if (EVENT_NAME.equals(event.getEventType().getName())) {
					count += 1;
				}
			}
			System.out.println(count + " " + EVENT_NAME + " events");
			if (expectEvents) {
				check("events are produced", 0 < count);
			} else {
				check("no events are produced", 0 == count);
			}
		} finally {
			Files.deleteIfExists(dumpFile);
		}
	}
}
