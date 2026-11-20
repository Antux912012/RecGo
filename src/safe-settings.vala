namespace RecGo {

    /**
     * Wrapper around GLib.Settings that gracefully falls back to in-memory
     * defaults when the installed GSettings schema is missing newer keys
     * (e.g. an outdated Flatpak wrapper schema) instead of aborting.
     */
    public class SafeSettings : GLib.Object {
        private GLib.Settings? inner = null;
        private GLib.SettingsSchema? schema = null;
        private GLib.HashTable<string, GLib.Variant> fallback_store;
        private GLib.GenericSet<string> warned_keys;

        public SafeSettings (string schema_id) {
            fallback_store = new GLib.HashTable<string, GLib.Variant> (GLib.str_hash, GLib.str_equal);
            warned_keys = new GLib.GenericSet<string> (GLib.str_hash, GLib.str_equal);

            var src = GLib.SettingsSchemaSource.get_default ();
            if (src != null) {
                schema = src.lookup (schema_id, true);
            }
            if (schema != null) {
                inner = new GLib.Settings (schema_id);
            } else {
                GLib.warning ("GSettings schema '%s' not found; using in-memory defaults.", schema_id);
            }
        }

        private bool has_key (string key) {
            if (inner != null && schema != null && schema.has_key (key)) {
                return true;
            }
            if (!warned_keys.contains (key)) {
                warned_keys.add (key);
                GLib.warning ("GSettings key '%s' missing from installed schema; using fallback default.", key);
            }
            return false;
        }

        public bool get_boolean (string key, bool fallback = false) {
            if (has_key (key)) {
                return inner.get_boolean (key);
            }
            unowned GLib.Variant? v = fallback_store.lookup (key);
            return (v != null) ? v.get_boolean () : fallback;
        }

        public void set_boolean (string key, bool val) {
            if (has_key (key)) {
                inner.set_boolean (key, val);
            } else {
                fallback_store.insert (key, new GLib.Variant.boolean (val));
            }
        }

        public double get_double (string key, double fallback = 0.0) {
            if (has_key (key)) {
                return inner.get_double (key);
            }
            unowned GLib.Variant? v = fallback_store.lookup (key);
            return (v != null) ? v.get_double () : fallback;
        }

        public void set_double (string key, double val) {
            if (has_key (key)) {
                inner.set_double (key, val);
            } else {
                fallback_store.insert (key, new GLib.Variant.double (val));
            }
        }

        public uint get_uint (string key, uint fallback = 0) {
            if (has_key (key)) {
                return inner.get_uint (key);
            }
            unowned GLib.Variant? v = fallback_store.lookup (key);
            return (v != null) ? v.get_uint32 () : fallback;
        }

        public void set_uint (string key, uint val) {
            if (has_key (key)) {
                inner.set_uint (key, val);
            } else {
                fallback_store.insert (key, new GLib.Variant.uint32 (val));
            }
        }

        public string get_string (string key, string fallback = "") {
            if (has_key (key)) {
                return inner.get_string (key);
            }
            unowned GLib.Variant? v = fallback_store.lookup (key);
            return (v != null) ? v.get_string () : fallback;
        }

        public void set_string (string key, string val) {
            if (has_key (key)) {
                inner.set_string (key, val);
            } else {
                fallback_store.insert (key, new GLib.Variant.string (val));
            }
        }

        public string[] get_strv (string key) {
            if (has_key (key)) {
                return inner.get_strv (key);
            }
            unowned GLib.Variant? v = fallback_store.lookup (key);
            return (v != null) ? v.dup_strv () : new string[0];
        }

        public void set_strv (string key, string[] val) {
            if (has_key (key)) {
                inner.set_strv (key, val);
            } else {
                fallback_store.insert (key, new GLib.Variant.strv (val));
            }
        }

        public void bind_boolean (string key,
                                  GLib.Object object,
                                  string property,
                                  GLib.SettingsBindFlags flags,
                                  bool fallback = false) {
            if (has_key (key)) {
                inner.bind (key, object, property, flags);
            } else {
                object.set (property, get_boolean (key, fallback));
                object.notify[property].connect (() => {
                    bool current = fallback;
                    object.get (property, out current);
                    set_boolean (key, current);
                });
            }
        }
    }
}
