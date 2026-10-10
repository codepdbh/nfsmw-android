package com.nfsmw.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.List;

/**
 * The graphics options of the launcher. Each one is a game cvar: GameActivity passes them on the command
 * line, which wins over nfsmw.toml. Values must be ones the cvar allows (nfsmw_ajustes_graficos.cpp and
 * friends), or the game ignores them.
 *
 * <p>Titles and labels are string resources so the launcher follows the device language
 * (see LauncherLocale); the game text language keeps its own option (nfsmw_idioma).
 */
final class GameOptions {
    static final class Option {
        final String key;
        final int titleRes;
        final String cvar;
        final String[] values;
        final int[] labelRes;
        final String defaultValue;
        final boolean languageNames;

        Option(String key, int titleRes, String cvar, String defaultValue, String[] values, int[] labelRes) {
            this(key, titleRes, cvar, defaultValue, values, labelRes, false);
        }

        Option(String key, int titleRes, String cvar, String defaultValue, String[] values, int[] labelRes,
                boolean languageNames) {
            this.key = key;
            this.titleRes = titleRes;
            this.cvar = cvar;
            this.defaultValue = defaultValue;
            this.values = values;
            this.labelRes = labelRes;
            this.languageNames = languageNames;
        }

        String title(Context context) {
            return context.getString(titleRes);
        }

        String label(Context context, String value) {
            if (languageNames) {
                String name = languageLabel(value);
                if (name != null) {
                    return name;
                }
                return context.getString(R.string.lang_edition);
            }
            for (int i = 0; i < values.length; i++) {
                if (values[i].equals(value)) {
                    return context.getString(labelRes[i]);
                }
            }
            return value;
        }

        String[] labels(Context context) {
            String[] out = new String[values.length];
            for (int i = 0; i < values.length; i++) {
                out[i] = label(context, values[i]);
            }
            return out;
        }
    }

    static final String RESOLUTION = "resolution";
    static final String FPS = "fps";
    static final String RENDERER = "renderer";

    static final Option[] ALL = {
            new Option(RENDERER, R.string.opt_renderer, "nfsmw_renderizador", "nativo",
                    new String[] {"nativo", "xenos"},
                    new int[] {R.string.renderer_native, R.string.renderer_compat}),
            new Option("gpu_stability", R.string.opt_stability, "nfsmw_consultas_oclusion", "auto",
                    new String[] {"auto", "off", "on"},
                    new int[] {R.string.stability_auto, R.string.stability_max,
                            R.string.stability_full}),
            new Option(RESOLUTION, R.string.opt_resolution, "nfsmw_resolucion_interna", "1280x720",
                    new String[] {"640x360", "1024x576", "1280x720", "1920x1080"},
                    new int[] {R.string.res_perf_max, R.string.res_perf, R.string.res_balanced,
                            R.string.res_quality}),
            new Option(FPS, R.string.opt_fps, "nfsmw_limite_fps", "60",
                    new String[] {"30", "60", "90", "120"},
                    new int[] {R.string.fps_battery, R.string.fps_60, R.string.fps_90,
                            R.string.fps_120}),
            new Option("aa", R.string.opt_aa, "nfsmw_antialiasing", "apagado",
                    new String[] {"apagado", "fxaa"},
                    new int[] {R.string.aa_off, R.string.aa_fxaa}),
            new Option("shadows", R.string.opt_shadows, "nfsmw_sombras_cada", "1",
                    new String[] {"1", "2"},
                    new int[] {R.string.shadows_each, R.string.shadows_half}),
            new Option("car_reflections", R.string.opt_car_refl, "nfsmw_cubemap_caras_max", "6",
                    new String[] {"6", "2", "1"},
                    new int[] {R.string.refl_high, R.string.refl_medium, R.string.refl_low}),
            new Option("road_reflection", R.string.opt_road_refl, "nfsmw_reflejo_carretera", "true",
                    new String[] {"true", "false"},
                    new int[] {R.string.switch_on, R.string.switch_off_perf}),
            new Option("sky", R.string.opt_sky, "nfsmw_resplandor_cielo", "natural",
                    new String[] {"original", "natural", "suave"},
                    new int[] {R.string.sky_original, R.string.sky_natural, R.string.sky_soft}),
            // The PAL discs carry the text of these ten languages (app/src/nfsmw_idioma.cpp). Speech and movies
            // stay in the disc's language. Default is the edition's language. Names stay in their own
            // language; they are not launcher translations.
            new Option("language", R.string.opt_language, "nfsmw_idioma", "-1",
                    new String[] {"-1", "4", "0", "1", "2", "3", "5", "6", "7", "12", "13"},
                    new int[] {R.string.lang_edition}, true),
            new Option("volume", R.string.opt_volume, "audio_ganancia_pct", "100",
                    new String[] {"100", "125", "150", "200"},
                    new int[] {R.string.vol_normal, R.string.vol_high, R.string.vol_higher,
                            R.string.vol_max}),
            new Option("filter", R.string.opt_filter, "nfsmw_posproceso", "apagado",
                    new String[] {"apagado", "cine", "vivo", "calido", "frio", "sepia", "noir", "crt"},
                    new int[] {R.string.filter_off, R.string.filter_cinema, R.string.filter_vivid,
                            R.string.filter_warm, R.string.filter_cold, R.string.filter_sepia,
                            R.string.filter_noir, R.string.filter_crt}),
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

    /** Game text language names stay in their own language; they are not launcher translations. */
    static String languageLabel(String value) {
        switch (value) {
            case "-1":
                return null;  // resolved with Context: R.string.lang_edition
            case "4":
                return "Español";
            case "0":
                return "English";
            case "1":
                return "Français";
            case "2":
                return "Deutsch";
            case "3":
                return "Italiano";
            case "5":
                return "Nederlands";
            case "6":
                return "Svenska";
            case "7":
                return "Dansk";
            case "12":
                return "Polski";
            case "13":
                return "Suomi";
            default:
                return value;
        }
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
