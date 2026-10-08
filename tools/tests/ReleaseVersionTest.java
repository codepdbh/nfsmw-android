package com.nfsmw.android;
public final class ReleaseVersionTest {
    static void check(boolean actual, boolean expected) { if (actual != expected) throw new AssertionError(); }
    public static void main(String[] args) {
        check(ReleaseVersion.newer("v0.5.5","0.5.4"),true);
        check(ReleaseVersion.newer("v0.10.0","0.5.4"),true);
        check(ReleaseVersion.newer("v1.0.0","0.5.4"),true);
        check(ReleaseVersion.newer("v0.5.4","0.5.4"),false);
        check(ReleaseVersion.newer("v0.3.8","0.5.4"),false);
        check(ReleaseVersion.newer("v0.5.4","0.5.5"),false);
        check(ReleaseVersion.newer("v0.6.0-beta","0.5.4"),false);
        check(ReleaseVersion.newer("999999999999999999.0.0","0.5.4"),false);
        check(ReleaseVersion.newer("release/latest","0.5.4"),false);
        check(ReleaseVersion.newer(null,"0.5.4"),false);
        System.out.println("Release comparison passed: upgrades, same version, downgrades, multi-digit versions, prereleases, malformed tags.");
    }
}
