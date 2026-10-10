package com.nfsmw.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Build;
import android.os.LocaleList;

import java.util.Locale;

/**
 * Launcher language: automatic (device language) plus manual override.
 *
 * <p>Automatic mode relies on Android resource qualifiers (values-es, values-tr,
 * values-fr, values-de, values-it, values-zh-rCN, values-zh-rTW): the system
 * picks the best match for the device language. A manual choice is stored in
 * SharedPreferences and applied in attachBaseContext of each activity.
 */
final class LauncherLocale {
    static final String AUTO = "auto";
    static final String TURKISH = "tr";
    static final String ENGLISH = "en";
    static final String SPANISH = "es";
    static final String FRENCH = "fr";
    static final String GERMAN = "de";
    static final String ITALIAN = "it";
    static final String CHINESE_SIMPLIFIED = "zh-CN";
    static final String CHINESE_TRADITIONAL = "zh-TW";

    static final String[] VALUES = {
        AUTO, TURKISH, ENGLISH, SPANISH, FRENCH, GERMAN, ITALIAN,
        CHINESE_SIMPLIFIED, CHINESE_TRADITIONAL,
    };

    private static final String PREFS = "nfsmw_locale";
    private static final String KEY = "launcher_lang";

    private LauncherLocale() {
    }

    static String get(Context context) {
        String value = prefs(context).getString(KEY, AUTO);
        for (String allowed : VALUES) {
            if (allowed.equals(value)) {
                return value;
            }
        }
        return AUTO;
    }

    static void set(Context context, String value) {
        prefs(context).edit().putString(KEY, value).apply();
    }

    static String label(Context context, String value) {
        switch (value) {
            case TURKISH:
                return "Türkçe";
            case ENGLISH:
                return "English";
            case SPANISH:
                return "Español";
            case FRENCH:
                return "Français";
            case GERMAN:
                return "Deutsch";
            case ITALIAN:
                return "Italiano";
            case CHINESE_SIMPLIFIED:
                return "简体中文";
            case CHINESE_TRADITIONAL:
                return "繁體中文";
            case AUTO:
            default:
                return context.getString(R.string.launcher_lang_auto);
        }
    }

    static Context apply(Context base) {
        String value = get(base);
        if (AUTO.equals(value)) {
            return base;
        }
        Locale locale = tagToLocale(value);
        Locale.setDefault(locale);
        Resources resources = base.getResources();
        Configuration config = new Configuration(resources.getConfiguration());
        config.setLocale(locale);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            config.setLocales(new LocaleList(locale));
        }
        return base.createConfigurationContext(config);
    }

    private static Locale tagToLocale(String value) {
        switch (value) {
            case TURKISH:
                return new Locale("tr");
            case ENGLISH:
                return Locale.ENGLISH;
            case SPANISH:
                return new Locale("es");
            case FRENCH:
                return Locale.FRENCH;
            case GERMAN:
                return Locale.GERMAN;
            case ITALIAN:
                return Locale.ITALIAN;
            case CHINESE_SIMPLIFIED:
                return Locale.SIMPLIFIED_CHINESE;
            case CHINESE_TRADITIONAL:
                return Locale.TRADITIONAL_CHINESE;
            default:
                return Locale.getDefault();
        }
    }

    private static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }
}
