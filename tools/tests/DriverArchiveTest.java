package com.nfsmw.android;
import java.io.*;
import java.nio.file.*;
import java.util.zip.*;

public final class DriverArchiveTest {
    static byte[] zip(String name, byte[] content) throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(output)) {
            zip.putNextEntry(new ZipEntry(name)); zip.write(content); zip.closeEntry();
        }
        return output.toByteArray();
    }
    static void rejects(String path, byte[] data) throws Exception {
        File root = Files.createTempDirectory("nfsmw-driver-test-").toFile();
        try {
            try { DriverArchive.extract(new ByteArrayInputStream(zip(path, data)), root); }
            catch (IOException expected) { return; }
            throw new AssertionError("Accepted invalid archive: " + path);
        } finally { DriverArchive.delete(root); }
    }
    public static void main(String[] args) throws Exception {
        rejects("../escape.so", new byte[1]); rejects("/absolute.so", new byte[1]);
        rejects("..\\escape.so", new byte[1]); rejects("meta.json", new byte[65537]);
        byte[] elf = new byte[64]; elf[0]=127; elf[1]='E'; elf[2]='L'; elf[3]='F';
        elf[4]=2; elf[5]=1; elf[16]=3; elf[18]=(byte)183;
        File root = Files.createTempDirectory("nfsmw-driver-test-").toFile();
        try {
            DriverArchive.extract(new ByteArrayInputStream(zip("wrapped/libvulkan_test.so", elf)), root);
            File library = new File(root,"wrapped/libvulkan_test.so"); DriverArchive.validateLibrary(library);
            elf[18]=62; Files.write(library.toPath(), elf);
            try { DriverArchive.validateLibrary(library); throw new AssertionError("Accepted x86 driver"); }
            catch (IOException expected) { }
        } finally { DriverArchive.delete(root); }
        System.out.println("Driver ZIP checks passed: wrapped package, ARM64 ELF, traversal, metadata limit, incompatible architecture.");
    }
}
