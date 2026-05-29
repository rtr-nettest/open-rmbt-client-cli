package at.rtr.rmbt.client;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

/** Persists the server-assigned client UUID to ~/.rmbt_client_uuid. */
final class UuidStore {

    private UuidStore() {}

    private static Path filePath() {
        String home = System.getenv("HOME");
        if (home == null) home = System.getenv("USERPROFILE");
        if (home == null) home = System.getProperty("user.home");
        return Path.of(home, ".rmbt_client_uuid");
    }

    /** Returns the stored UUID, or null if none exists yet. */
    static String load() {
        try {
            String s = Files.readString(filePath()).strip();
            return s.isEmpty() ? null : s;
        } catch (IOException e) {
            return null;
        }
    }

    /** Persist a server-assigned UUID. */
    static void save(String uuid) {
        Path path = filePath();
        try {
            Files.writeString(path, uuid + "\n");
            System.out.printf("Client UUID saved: %s%n  (%s)%n%n", uuid, path);
        } catch (IOException e) {
            System.err.println("Warning: could not save UUID to " + path + ": " + e.getMessage());
        }
    }
}
