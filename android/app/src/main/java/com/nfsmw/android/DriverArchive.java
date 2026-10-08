package com.nfsmw.android;

import java.io.*;
import java.nio.file.Files;
import java.util.HashSet;
import java.util.Set;
import java.util.zip.*;

/** Bounded driver ZIP extraction. No Android dependencies so malformed packages can be tested on the host. */
final class DriverArchive {
    private static final long LIMIT = 256L * 1024 * 1024;
    static void extract(InputStream input, File root) throws IOException {
        String prefix = root.getCanonicalPath() + File.separator;
        Set<String> seen = new HashSet<>();
        long total = 0;
        int entries = 0;
        try (ZipInputStream zip = new ZipInputStream(input)) {
            ZipEntry entry;
            byte[] buffer = new byte[65536];
            while ((entry = zip.getNextEntry()) != null) {
                if (++entries > 1024) throw new IOException("El ZIP contiene demasiados archivos.");
                String name = entry.getName();
                File file = new File(root, name);
                if (name.startsWith("/") || name.contains("\\") || name.contains(":") || name.split("/").length > 32 ||
                        !file.getCanonicalPath().startsWith(prefix) || !seen.add(file.getCanonicalPath()))
                    throw new IOException("El ZIP contiene rutas no válidas o repetidas.");
                if (entry.isDirectory()) {
                    if (!file.isDirectory() && !file.mkdirs()) throw new IOException("No se pudo crear la carpeta.");
                    continue;
                }
                if (!file.getParentFile().isDirectory() && !file.getParentFile().mkdirs())
                    throw new IOException("No se pudo crear la carpeta.");
                long size = 0;
                try (FileOutputStream output = new FileOutputStream(file)) {
                    int n;
                    while ((n = zip.read(buffer)) != -1) {
                        size += n; total += n;
                        if (size > LIMIT / 2 || total > LIMIT || (file.getName().equals("meta.json") && size > 65536))
                            throw new IOException("El paquete de driver es demasiado grande.");
                        output.write(buffer, 0, n);
                    }
                }
            }
        }
    }

    static void validateLibrary(File library) throws IOException {
        byte[] h = new byte[64];
        try (DataInputStream input = new DataInputStream(new FileInputStream(library))) { input.readFully(h); }
        if (h[0] != 127 || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' || h[4] != 2 || h[5] != 1 ||
                h[16] != 3 || h[17] != 0 || (h[18] & 255) != 183 || h[19] != 0)
            throw new IOException("El driver debe ser una biblioteca Android ARM64 (.so), no de PC ni de 32 bits.");
    }

    static void delete(File file) throws IOException {
        if (Files.isSymbolicLink(file.toPath())) throw new IOException("Enlace inesperado en el paquete.");
        File[] children = file.listFiles();
        if (children != null) for (File child : children) delete(child);
        if (file.exists() && !file.delete()) throw new IOException("No se pudo eliminar " + file.getName());
    }
}
