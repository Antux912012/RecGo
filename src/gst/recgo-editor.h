#pragma once

#include "recgo-codecs.h"
#include <glib-object.h>

G_BEGIN_DECLS

typedef struct {
    guint64 start_ms;
    guint64 end_ms;
} RecGoEditSegment;

#define RECGO_TYPE_EDITOR (recgo_editor_get_type ())
G_DECLARE_FINAL_TYPE (RecGoEditor, recgo_editor, RECGO, EDITOR, GObject)

RecGoEditor *recgo_editor_new (void);

gboolean recgo_editor_export_async (RecGoEditor *self,
                                    const gchar *input_path,
                                    const gchar *output_path,
                                    const RecGoEditSegment *segments,
                                    guint n_segments,
                                    RecGoVideoCodec codec,
                                    RecGoContainerFormat container,
                                    GError **error);

void recgo_editor_cancel (RecGoEditor *self);

G_END_DECLS
