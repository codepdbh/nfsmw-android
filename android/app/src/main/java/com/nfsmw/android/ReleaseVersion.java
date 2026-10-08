package com.nfsmw.android;

/** Compare stable release tags numerically (0.10.0 > 0.5.4), never by text or publication date. */
final class ReleaseVersion {
    private static int[] parse(String version) {
        if (version == null || !version.matches("v?[0-9]{1,9}\\.[0-9]{1,9}\\.[0-9]{1,9}")) return null;
        String[] parts = version.replaceFirst("^v", "").split("\\.");
        return new int[] {Integer.parseInt(parts[0]), Integer.parseInt(parts[1]), Integer.parseInt(parts[2])};
    }
    static boolean newer(String remote, String installed) {
        int[] a = parse(remote), b = parse(installed);
        if (a == null || b == null) return false;
        for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
        return false;
    }
}
