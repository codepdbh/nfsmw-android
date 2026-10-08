package com.nfsmw.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.List;

/**
 * The graphics options of the launcher. Each one is a game cvar: GameActivity passes them on the command
 * line, which wins over nfsmw.toml. Values must be ones the cvar allows (nfsmw_ajustes_graficos.cpp and
 * friends), or the game ignores them.
 */
final class GameOptions {
    static final class Option {
        final String key;
        final String title;
        final String cvar;
        final String[] values;
        final String[] labels;
        final String defaultValue;

        Option(String key, String title, String cvar, String defaultValue, String[] values, String[] labels) {
            this.key = key;
            this.title = title;
            this.cvar = cvar;
            this.defaultValue = defaultValue;
            this.values = values;
            this.labels = labels;
        }

        String label(String value) {
            for (int i = 0; i < values.length; i++) {
                if (values[i].equals(value)) {
                    return labels[i];
                }
            }
            return value;
        }
    }

    static final String RESOLUTION = "resolution";
    static final String FPS = "fps";
    static final String RENDERER = "renderer";

    static final Option[] ALL = {
            new Option(RENDERER, "Renderizador", "nfsmw_renderizador", "nativo",
                    new String[] {"nativo", "xenos"},
                    new String[] {"Nativo", "Compatibilidad · experimental"}),
            new Option("gpu_stability", "Estabilidad gráfica", "nfsmw_consultas_oclusion", "auto",
                    new String[] {"auto", "off", "on"},
                    new String[] {"Automática · protección Mali", "Máxima compatibilidad · sin destello solar",
                            "Efectos completos · experimental"}),
            new Option(RESOLUTION, "Resolución interna", "nfsmw_resolucion_interna", "1280x720",
                    new String[] {"640x360", "1024x576", "1280x720", "1920x1080"},
                    new String[] {"640x360 · máximo rendimiento", "1024x576 · rendimiento", "1280x720 · equilibrado",
                            "1920x1080 · máxima calidad"}),
            new Option(FPS, "Límite de FPS", "nfsmw_limite_fps", "60",
                    new String[] {"30", "60", "90", "120"},
                    new String[] {"30 FPS · ahorra batería", "60 FPS", "90 FPS · experimental",
                            "120 FPS · experimental"}),
            new Option("aa", "Antialiasing", "nfsmw_antialiasing", "apagado",
                    new String[] {"apagado", "fxaa"},
                    new String[] {"Desactivado", "FXAA"}),
            new Option("shadows", "Sombras", "nfsmw_sombras_cada", "1",
                    new String[] {"1", "2"},
                    new String[] {"Cada fotograma", "Cada 2 fotogramas · más rendimiento"}),
            new Option("car_reflections", "Reflejos del coche", "nfsmw_cubemap_caras_max", "6",
                    new String[] {"6", "2", "1"},
                    new String[] {"Altos", "Medios", "Bajos · más rendimiento"}),
            new Option("road_reflection", "Reflejo del asfalto mojado", "nfsmw_reflejo_carretera", "true",
                    new String[] {"true", "false"},
                    new String[] {"Activado", "Desactivado · más rendimiento"}),
            new Option("sky", "Resplandor del cielo", "nfsmw_resplandor_cielo", "natural",
                    new String[] {"original", "natural", "suave"},
                    new String[] {"Original (Xbox 360)", "Natural", "Suave"}),
            // The PAL discs carry the text of these ten languages (app/src/nfsmw_idioma.cpp). Speech and movies
            // stay in the disc's language.
            new Option("language", "Idioma de los textos", "nfsmw_idioma", "-1",
                    new String[] {"-1", "4", "0", "1", "2", "3", "5", "6", "7", "12", "13"},
                    new String[] {"El de la edición", "Español", "English", "Français", "Deutsch", "Italiano",
                            "Nederlands", "Svenska", "Dansk", "Polski", "Suomi"}),
            new Option("volume", "Volumen del juego", "audio_ganancia_pct", "100",
                    new String[] {"100", "125", "150", "200"},
                    new String[] {"Normal", "Alto", "Muy alto", "Máximo · puede saturar"}),
            new Option("filter", "Filtro de imagen", "nfsmw_posproceso", "apagado",
                    new String[] {"apagado", "cine", "vivo", "calido", "frio", "sepia", "noir", "crt"},
                    new String[] {"Sin filtro", "Cine", "Vivo", "Cálido", "Frío", "Sepia", "Blanco y negro",
                            "CRT"}),
    };

    private static final String PREFS = "nfsmw_game";

    private GameOptions() {
    }

    static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static Option find(String key) {
        for (Option o : ALL) {
            if (o.key.equals(key)) {
                return o;
            }
        }
        throw new IllegalArgumentException(key);
    }

    static String get(Context context, String key) {
        Option o = find(key);
        String value = prefs(context).getString(key, o.defaultValue);
        for (String allowed : o.values) {
            if (allowed.equals(value)) {
                return value;
            }
        }
        return o.defaultValue;
    }

    static void set(Context context, String key, String value) {
        prefs(context).edit().putString(key, value).apply();
    }

    static List<String> arguments(Context context) {
        List<String> args = new ArrayList<>();
        for (Option o : ALL) {
            args.add("--" + o.cvar + "=" + get(context, o.key));
        }
        if ("xenos".equals(get(context, RENDERER))) {
            // Use ordinary descriptor sets and host framebuffers on older drivers.
            // Xenos also decompresses unsupported BC texture formats on the GPU.
            args.add("--vulkan_native_shader_features=false");
            args.add("--render_target_path_vulkan=fbo");
            args.add("--vulkan_require_geometry_shader=false");
            args.add("--vulkan_require_fill_mode_non_solid=false");
            args.add("--async_shader_compilation=false");
            args.add("--nfsmw_d3d_registros_nativo=false");
            args.add("--nfsmw_d3d_marcador=false");
            args.add("--nfsmw_d3d_marcador_registro=false");
            args.add("--nfsmw_d3d_efectos_nativo=false");
            args.add("--nfsmw_render_sin_mosaico=false");
            args.add("--nfsmw_material_nativo=false");
            args.add("--nfsmw_visible_nativo=false");
            args.add("--nfsmw_matrices_nativo=false");
            args.add("--nfsmw_eview_nativo=false");
            args.add("--nfsmw_escenario_nativo=false");
            args.add("--nfsmw_efecto_pasada_nativo=false");
            args.add("--nfsmw_pegamento_nativo=false");
        }
        GpuDrivers.arguments(context, args);
        return args;
    }
}
