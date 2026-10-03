package at.rtr.rmbt.client;

/**
 * {@code loopmode_info} payload that groups several individual measurements into
 * one <em>loop</em> on the control server.
 *
 * <p>Serialized exactly as the server's {@code LoopModeSettings}: {@code max_delay},
 * {@code max_movement}, {@code max_tests}, {@code test_counter}, {@code loop_uuid}.
 * See {@code doc/json_interface.md}.
 *
 * @param maxDelay    max waiting time between iterations, in minutes
 * @param maxMovement max movement in metres before a new loop; always 0 here
 *                    (this client does not perform signal/GPS measurement)
 * @param maxTests    planned number of tests in the loop; 0 when unbounded/unknown
 * @param testCounter 0-based index of this iteration within the loop
 * @param loopUuid    server loop UUID; {@code null} on the first iteration (the
 *                    server mints one and returns it), echoed thereafter
 */
record LoopModeInfo(int maxDelay, int maxMovement, int maxTests, int testCounter, String loopUuid) {}
