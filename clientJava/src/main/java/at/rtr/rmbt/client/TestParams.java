package at.rtr.rmbt.client;

/** Parameters returned by the control server for a single test session. */
record TestParams(
    String  token,
    String  testUuid,
    String  openTestUuid,
    String  serverAddr,
    int     serverPort,
    boolean encryption,
    int     duration,
    int     numThreads,
    int     waitSecs,
    String  serverType
) {}
