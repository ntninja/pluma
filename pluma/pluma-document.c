/*
 * pluma-document.c
 * This file is part of pluma
 *
 * Copyright (C) 1998, 1999 Alex Roberts, Evan Lawrence
 * Copyright (C) 2000, 2001 Chema Celorio, Paolo Maggi
 * Copyright (C) 2002-2005 Paolo Maggi
 * Copyright (C) 2012-2021 MATE Developers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/*
 * Modified by the pluma Team, 1998-2005. See the AUTHORS file for a
 * list of people on the pluma Team.
 * See the ChangeLog files for a list of changes.
 *
 * $Id$
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "pluma-settings.h"
#include "pluma-document.h"
#include "pluma-debug.h"
#include "pluma-utils.h"
#include "pluma-language-manager.h"
#include "pluma-style-scheme-manager.h"
#include "pluma-document-loader.h"
#include "pluma-document-saver.h"
#include "pluma-enum-types.h"
#include "plumatextregion.h"

#ifndef ENABLE_GVFS_METADATA
#include "pluma-metadata-manager.h"
#else
#define METADATA_QUERY "metadata::*"
#endif

#undef ENABLE_PROFILE

#ifdef ENABLE_PROFILE
#define PROFILE(x) x
#else
#define PROFILE(x)
#endif

PROFILE (static GTimer *timer = NULL)

#ifdef MAXPATHLEN
#define PLUMA_MAX_PATH_LEN  MAXPATHLEN
#elif defined (PATH_MAX)
#define PLUMA_MAX_PATH_LEN  PATH_MAX
#else
#define PLUMA_MAX_PATH_LEN  2048
#endif

/* undo https://gitlab.gnome.org/GNOME/gtksourceview/-/commit/b3dffc39 */
#undef GTK_SOURCE_CHECK_VERSION
#define GTK_SOURCE_CHECK_VERSION(major, minor, micro) \
        (GTK_SOURCE_MAJOR_VERSION > (major) || \
        (GTK_SOURCE_MAJOR_VERSION == (major) && GTK_SOURCE_MINOR_VERSION > (minor)) || \
        (GTK_SOURCE_MAJOR_VERSION == (major) && GTK_SOURCE_MINOR_VERSION == (minor) && \
         GTK_SOURCE_MICRO_VERSION >= (micro)))

static void	pluma_document_load_real	(PlumaDocument          *doc,
						 const gchar            *uri,
						 const PlumaEncoding    *encoding,
						 gint                    line_pos,
						 gboolean                create);
static void	pluma_document_save_real	(PlumaDocument          *doc,
						 const gchar            *uri,
						 const PlumaEncoding    *encoding,
						 PlumaDocumentSaveFlags  flags);
static void	to_search_region_range 		(PlumaDocument *doc,
						 GtkTextIter   *start,
						 GtkTextIter   *end);
static void 	insert_text_cb		 	(PlumaDocument *doc,
						 GtkTextIter   *pos,
						 const gchar   *text,
						 gint           length);

static void	delete_range_cb 		(PlumaDocument *doc,
						 GtkTextIter   *start,
						 GtkTextIter   *end);

struct _PlumaDocumentPrivate
{
	GSettings   *editor_settings;

	gchar	    *uri;
	gint 	     untitled_number;
	gchar       *short_name;

	GFileInfo   *metadata_info;

	const PlumaEncoding *encoding;

	gchar	    *content_type;

	gint64       mtime;
	gint64       time_of_last_save_or_load;

	guint        search_flags;
	gchar       *search_text;
	gchar       *last_replace_text;
	gint	     num_of_lines_search_text;

	PlumaDocumentNewlineType newline_type;
	gboolean hide_trailing_newline;

	/* Temp data while loading */
	PlumaDocumentLoader *loader;
	gboolean             create; /* Create file if uri points
	                              * to a non existing file */
	const PlumaEncoding *requested_encoding;
	gint                 requested_line_pos;

	/* Saving stuff */
	PlumaDocumentSaver *saver;

	/* Search highlighting support variables */
	PlumaTextRegion *to_search_region;
	GtkTextTag      *found_tag;

	/* Mount operation factory */
	PlumaMountOperationFactory  mount_operation_factory;
	gpointer		    mount_operation_userdata;

	gint readonly : 1;
	gint last_save_was_manually : 1;
	gint language_set_by_user : 1;
	gint stop_cursor_moved_emission : 1;
	gint dispose_has_run : 1;
};

enum {
	PROP_0,

	PROP_URI,
	PROP_SHORTNAME,
	PROP_CONTENT_TYPE,
	PROP_MIME_TYPE,
	PROP_READ_ONLY,
	PROP_ENCODING,
	PROP_CAN_SEARCH_AGAIN,
	PROP_ENABLE_SEARCH_HIGHLIGHTING,
	PROP_NEWLINE_TYPE,
	PROP_HIDE_TRAILING_NEWLINE,
};

enum {
	CURSOR_MOVED,
	LOAD,
	LOADING,
	LOADED,
	SAVE,
	SAVING,
	SAVED,
	SEARCH_HIGHLIGHT_UPDATED,
	LAST_SIGNAL
};

static guint document_signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE_WITH_PRIVATE (PlumaDocument, pluma_document, GTK_SOURCE_TYPE_BUFFER)

GQuark
pluma_document_error_quark (void)
{
	static GQuark quark = 0;

	if (G_UNLIKELY (quark == 0))
		quark = g_quark_from_static_string ("pluma_io_load_error");

	return quark;
}

static GHashTable *allocated_untitled_numbers = NULL;

static gint
get_untitled_number (void)
{
	gint i = 1;

	if (allocated_untitled_numbers == NULL)
		allocated_untitled_numbers = g_hash_table_new (NULL, NULL);

	g_return_val_if_fail (allocated_untitled_numbers != NULL, -1);

	while (TRUE)
	{
		if (g_hash_table_lookup (allocated_untitled_numbers, GINT_TO_POINTER (i)) == NULL)
		{
			g_hash_table_insert (allocated_untitled_numbers,
					     GINT_TO_POINTER (i),
					     GINT_TO_POINTER (i));

			return i;
		}

		++i;
	}
}

static void
release_untitled_number (gint n)
{
	g_return_if_fail (allocated_untitled_numbers != NULL);

	g_hash_table_remove (allocated_untitled_numbers, GINT_TO_POINTER (n));
}

static void
pluma_document_dispose (GObject *object)
{
	PlumaDocument *doc = PLUMA_DOCUMENT (object);

	pluma_debug (DEBUG_DOCUMENT);

	/* Metadata must be saved here and not in finalize
	 * because the language is gone by the time finalize runs.
	 * beside if some plugin prevents proper finalization by
	 * holding a ref to the doc, we still save the metadata */
	if ((!doc->priv->dispose_has_run) && (doc->priv->uri != NULL))
	{
		GtkTextIter iter;
		gchar *position;
		const gchar *language = NULL;

		if (doc->priv->language_set_by_user)
		{
			GtkSourceLanguage *lang;

			lang = pluma_document_get_language (doc);

			if (lang == NULL)
				language = "_NORMAL_";
			else
				language = gtk_source_language_get_id (lang);
		}

		gtk_text_buffer_get_iter_at_mark (
				GTK_TEXT_BUFFER (doc),
				&iter,
				gtk_text_buffer_get_insert (GTK_TEXT_BUFFER (doc)));

		position = g_strdup_printf ("%d",
					    gtk_text_iter_get_offset (&iter));

		if (language == NULL)
			pluma_document_set_metadata (doc, PLUMA_METADATA_ATTRIBUTE_POSITION,
						     position, NULL);
		else
			pluma_document_set_metadata (doc, PLUMA_METADATA_ATTRIBUTE_POSITION,
						     position, PLUMA_METADATA_ATTRIBUTE_LANGUAGE,
						     language, NULL);
		g_free (position);
	}

	if (doc->priv->loader)
	{
		g_object_unref (doc->priv->loader);
		doc->priv->loader = NULL;
	}

	if (doc->priv->metadata_info != NULL)
	{
		g_object_unref (doc->priv->metadata_info);
		doc->priv->metadata_info = NULL;
	}

	g_clear_object (&doc->priv->editor_settings);

	doc->priv->dispose_has_run = TRUE;

	G_OBJECT_CLASS (pluma_document_parent_class)->dispose (object);
}

static void
pluma_document_finalize (GObject *object)
{
	PlumaDocument *doc = PLUMA_DOCUMENT (object);

	pluma_debug (DEBUG_DOCUMENT);

	if (doc->priv->untitled_number > 0)
	{
		g_return_if_fail (doc->priv->uri == NULL);
		release_untitled_number (doc->priv->untitled_number);
	}

	g_free (doc->priv->uri);
	g_free (doc->priv->content_type);
	g_free (doc->priv->search_text);
	g_free (doc->priv->last_replace_text);

	if (doc->priv->to_search_region != NULL)
	{
		/* we can't delete marks if we're finalizing the buffer */
		pluma_text_region_destroy (doc->priv->to_search_region, FALSE);
	}

	G_OBJECT_CLASS (pluma_document_parent_class)->finalize (object);
}

static void
pluma_document_get_property (GObject    *object,
			     guint       prop_id,
			     GValue     *value,
			     GParamSpec *pspec)
{
	PlumaDocument *doc = PLUMA_DOCUMENT (object);

	switch (prop_id)
	{
		case PROP_URI:
			g_value_set_string (value, doc->priv->uri);
			break;
		case PROP_SHORTNAME:
			g_value_take_string (value, pluma_document_get_short_name_for_display (doc));
			break;
		case PROP_CONTENT_TYPE:
			g_value_take_string (value, pluma_document_get_content_type (doc));
			break;
		case PROP_MIME_TYPE:
			g_value_take_string (value, pluma_document_get_mime_type (doc));
			break;
		case PROP_READ_ONLY:
			g_value_set_boolean (value, doc->priv->readonly);
			break;
		case PROP_ENCODING:
			g_value_set_boxed (value, doc->priv->encoding);
			break;
		case PROP_CAN_SEARCH_AGAIN:
			g_value_set_boolean (value, pluma_document_get_can_search_again (doc));
			break;
		case PROP_ENABLE_SEARCH_HIGHLIGHTING:
			g_value_set_boolean (value, pluma_document_get_enable_search_highlighting (doc));
			break;
		case PROP_NEWLINE_TYPE:
			g_value_set_enum (value, doc->priv->newline_type);
			break;
		case PROP_HIDE_TRAILING_NEWLINE:
			g_value_set_boolean (value, doc->priv->hide_trailing_newline);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
pluma_document_set_property (GObject      *object,
			     guint         prop_id,
			     const GValue *value,
			     GParamSpec   *pspec)
{
	PlumaDocument *doc = PLUMA_DOCUMENT (object);

	switch (prop_id)
	{
		case PROP_ENABLE_SEARCH_HIGHLIGHTING:
			pluma_document_set_enable_search_highlighting (doc,
								       g_value_get_boolean (value));
			break;
		case PROP_NEWLINE_TYPE:
			pluma_document_set_newline_type (doc,
							 g_value_get_enum (value));
			break;
		case PROP_SHORTNAME:
			pluma_document_set_short_name_for_display (doc,
			                                           g_value_get_string (value));
			break;
		case PROP_CONTENT_TYPE:
			pluma_document_set_content_type (doc,
			                                 g_value_get_string (value));
			break;
		case PROP_HIDE_TRAILING_NEWLINE:
			doc->priv->hide_trailing_newline = g_value_get_boolean (value);
			/* XXX: This should also change whether newline is visible to the user
			        or not (ie: add or remove the newline from the buffer). Not
			        really important unless this property is actually exposed in
			        the user interface though. */
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
emit_cursor_moved (PlumaDocument *doc)
{
	if (!doc->priv->stop_cursor_moved_emission)
	{
		g_signal_emit (doc,
			       document_signals[CURSOR_MOVED],
			       0);
	}
}

static void
pluma_document_mark_set (GtkTextBuffer     *buffer,
                         const GtkTextIter *iter,
                         GtkTextMark       *mark)
{
	PlumaDocument *doc = PLUMA_DOCUMENT (buffer);

	if (GTK_TEXT_BUFFER_CLASS (pluma_document_parent_class)->mark_set)
		GTK_TEXT_BUFFER_CLASS (pluma_document_parent_class)->mark_set (buffer,
									       iter,
									       mark);

	if (mark == gtk_text_buffer_get_insert (buffer))
	{
		emit_cursor_moved (doc);
	}
}

static void
pluma_document_changed (GtkTextBuffer *buffer)
{
	emit_cursor_moved (PLUMA_DOCUMENT (buffer));

	GTK_TEXT_BUFFER_CLASS (pluma_document_parent_class)->changed (buffer);
}

static void
pluma_document_class_init (PlumaDocumentClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkTextBufferClass *buf_class = GTK_TEXT_BUFFER_CLASS (klass);

	object_class->dispose = pluma_document_dispose;
	object_class->finalize = pluma_document_finalize;
	object_class->get_property = pluma_document_get_property;
	object_class->set_property = pluma_document_set_property;

	buf_class->mark_set = pluma_document_mark_set;
	buf_class->changed = pluma_document_changed;

	klass->load = pluma_document_load_real;
	klass->save = pluma_document_save_real;

	g_object_class_install_property (object_class, PROP_URI,
					 g_param_spec_string ("uri",
							      "URI",
							      "The document's URI",
							      NULL,
							      G_PARAM_READABLE |
							      G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class, PROP_SHORTNAME,
					 g_param_spec_string ("shortname",
							      "Short Name",
							      "The document's short name",
							      NULL,
							      G_PARAM_READWRITE |
							      G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class, PROP_CONTENT_TYPE,
					 g_param_spec_string ("content-type",
							      "Content Type",
							      "The document's Content Type",
							      NULL,
							      G_PARAM_READWRITE |
							      G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class, PROP_MIME_TYPE,
					 g_param_spec_string ("mime-type",
							      "MIME Type",
							      "The document's MIME Type",
							      "text/plain",
							      G_PARAM_READABLE |
							      G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class, PROP_READ_ONLY,
					 g_param_spec_boolean ("read-only",
							       "Read Only",
							       "Whether the document is read only or not",
							       FALSE,
							       G_PARAM_READABLE |
							       G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class, PROP_ENCODING,
					 g_param_spec_boxed ("encoding",
							     "Encoding",
							     "The PlumaEncoding used for the document",
							     PLUMA_TYPE_ENCODING,
							     G_PARAM_READABLE |
							     G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class, PROP_CAN_SEARCH_AGAIN,
					 g_param_spec_boolean ("can-search-again",
							       "Can search again",
							       "Whether it's possible to search again in the document",
							       FALSE,
							       G_PARAM_READABLE |
							       G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class, PROP_ENABLE_SEARCH_HIGHLIGHTING,
					 g_param_spec_boolean ("enable-search-highlighting",
							       "Enable Search Highlighting",
							       "Whether all the occurrences of the searched string must be highlighted",
							       FALSE,
							       G_PARAM_READWRITE |
							       G_PARAM_STATIC_STRINGS));

	/**
	 * PlumaDocument:newline-type:
	 *
	 * The :newline-type property determines what is considered
	 * as a line ending when saving the document
	 */
	g_object_class_install_property (object_class, PROP_NEWLINE_TYPE,
	                                 g_param_spec_enum ("newline-type",
	                                                    "Newline type",
	                                                    "The accepted types of line ending",
	                                                    PLUMA_TYPE_DOCUMENT_NEWLINE_TYPE,
	                                                    PLUMA_DOCUMENT_NEWLINE_TYPE_LF,
	                                                    G_PARAM_READWRITE |
	                                                    G_PARAM_STATIC_NAME |
	                                                    G_PARAM_STATIC_BLURB));

	/**
	 * PlumaDocument:hide-trailing-newline:
	 *
	 * The :hide-trailing-newline property determines whether the final newline
	 * in the document (if any) is stripped on loading and automatically readded
	 * on saving the document
	 */
	g_object_class_install_property (object_class, PROP_HIDE_TRAILING_NEWLINE,
	                                 g_param_spec_boolean ("hide-trailing-newline",
	                                                       "Hide Trailing Newline",
	                                                       "Drop trailing newline from input and add it on output",
	                                                       TRUE,
	                                                       G_PARAM_READWRITE |
	                                                       G_PARAM_STATIC_STRINGS));

	/* This signal is used to update the cursor position is the statusbar,
	 * it's emitted either when the insert mark is moved explicitely or
	 * when the buffer changes (insert/delete).
	 * We prevent the emission of the signal during replace_all to
	 * improve performance.
	 */
	document_signals[CURSOR_MOVED] =
   		g_signal_new ("cursor-moved",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PlumaDocumentClass, cursor_moved),
			      NULL, NULL, NULL,
			      G_TYPE_NONE,
			      0);

	/**
	 * PlumaDocument::load:
	 * @document: the #PlumaDocument.
	 * @uri: the uri where to load the document from.
	 * @encoding: the #PlumaEncoding to encode the document.
	 * @line_pos: the line to show.
	 * @create: whether the document should be created if it doesn't exist.
	 *
	 * The "load" signal is emitted when a document is loaded.
	 */
	document_signals[LOAD] =
		g_signal_new ("load",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PlumaDocumentClass, load),
			      NULL, NULL, NULL,
			      G_TYPE_NONE,
			      4,
			      G_TYPE_STRING,
			      /* we rely on the fact that the PlumaEncoding pointer stays
			       * the same forever */
			      PLUMA_TYPE_ENCODING | G_SIGNAL_TYPE_STATIC_SCOPE,
			      G_TYPE_INT,
			      G_TYPE_BOOLEAN);


	document_signals[LOADING] =
		g_signal_new ("loading",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PlumaDocumentClass, loading),
			      NULL, NULL, NULL,
			      G_TYPE_NONE,
			      2,
			      G_TYPE_UINT64,
			      G_TYPE_UINT64);

	document_signals[LOADED] =
   		g_signal_new ("loaded",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PlumaDocumentClass, loaded),
			      NULL, NULL, NULL,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_POINTER);

	/**
	 * PlumaDocument::save:
	 * @document: the #PlumaDocument.
	 * @uri: the uri where the document is about to be saved.
	 * @encoding: the #PlumaEncoding used to save the document.
	 * @flags: the #PlumaDocumentSaveFlags for the save operation.
	 *
	 * The "save" signal is emitted when the document is saved.
	 */
	document_signals[SAVE] =
		g_signal_new ("save",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PlumaDocumentClass, save),
			      NULL, NULL, NULL,
			      G_TYPE_NONE,
			      3,
			      G_TYPE_STRING,
			      /* we rely on the fact that the PlumaEncoding pointer stays
			       * the same forever */
			      PLUMA_TYPE_ENCODING | G_SIGNAL_TYPE_STATIC_SCOPE,
			      PLUMA_TYPE_DOCUMENT_SAVE_FLAGS);

	document_signals[SAVING] =
   		g_signal_new ("saving",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PlumaDocumentClass, saving),
			      NULL, NULL, NULL,
			      G_TYPE_NONE,
			      2,
			      G_TYPE_UINT64,
			      G_TYPE_UINT64);

	document_signals[SAVED] =
   		g_signal_new ("saved",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PlumaDocumentClass, saved),
			      NULL, NULL, NULL,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_POINTER);

	document_signals[SEARCH_HIGHLIGHT_UPDATED] =
	    	g_signal_new ("search_highlight_updated",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (PlumaDocumentClass, search_highlight_updated),
			      NULL, NULL, NULL,
			      G_TYPE_NONE,
			      2,
			      GTK_TYPE_TEXT_ITER | G_SIGNAL_TYPE_STATIC_SCOPE,
			      GTK_TYPE_TEXT_ITER | G_SIGNAL_TYPE_STATIC_SCOPE);
}

#if !GTK_SOURCE_CHECK_VERSION(4, 3, 1)
static gboolean
file_with_bom (GFile *file)
{
	FILE    *testfile;
	gchar    c;
	int      i;
	gchar    bom[3];
	gchar   *file_path;

	bom[0] = bom[1] = bom[2] = 0;

	file_path = g_file_get_path (file);

	testfile = fopen (file_path, "r");

	g_free (file_path);

	if (testfile == NULL)
	{
		perror ("fopen");
		return FALSE;
	}

	for (i = 0; i < 3; i++)
	{
		c = fgetc (testfile);

		if (c == EOF)
			break;
		else
			bom[i] = c;
	}

	fclose (testfile);

	if ((bom[0] == '\357') &&
	    (bom[1] == '\273') &&
	    (bom[2] == '\277'))
		return TRUE;
	else
		return FALSE;
}
#endif

static void
set_language (PlumaDocument     *doc,
              GtkSourceLanguage *lang,
              gboolean           set_by_user)
{
	GtkSourceLanguage *old_lang;

#if !GTK_SOURCE_CHECK_VERSION(4, 3, 1)
	const gchar       *new_lang_id;
	const gchar       *bom_langs[] = {
		"asp", "dtl", "docbook", "html", "mxml", "mallard", "markdown",
		"mediawiki", "php", "tera", "xml", "xslt", NULL
	};
	gboolean is_bom_lang = FALSE;
#endif

	pluma_debug (DEBUG_DOCUMENT);

	old_lang = gtk_source_buffer_get_language (GTK_SOURCE_BUFFER (doc));

	if (old_lang == lang)
		return;

#if !GTK_SOURCE_CHECK_VERSION(4, 3, 1)
	new_lang_id = gtk_source_language_get_id (lang);
	if (new_lang_id)
		is_bom_lang = g_strv_contains (bom_langs, new_lang_id);

	if (is_bom_lang)
	{
		GFile *file;

		file = pluma_document_get_location (doc);
		if (file)
		{
			if (!file_with_bom (file))
				gtk_source_buffer_set_language (GTK_SOURCE_BUFFER (doc), lang);

			g_object_unref (file);
		}
		else
			gtk_source_buffer_set_language (GTK_SOURCE_BUFFER (doc), lang);
	}
	else
#endif
		gtk_source_buffer_set_language (GTK_SOURCE_BUFFER (doc), lang);

	if (lang != NULL)
	{
		gboolean syntax_hl;

		syntax_hl = g_settings_get_boolean (doc->priv->editor_settings,
						    PLUMA_SETTINGS_SYNTAX_HIGHLIGHTING);

		gtk_source_buffer_set_highlight_syntax (GTK_SOURCE_BUFFER (doc),
							syntax_hl);
	}
	else
		gtk_source_buffer_set_highlight_syntax (GTK_SOURCE_BUFFER (doc),
				 FALSE);

	if (set_by_user && (doc->priv->uri != NULL))
	{
		pluma_document_set_metadata (doc, PLUMA_METADATA_ATTRIBUTE_LANGUAGE,
			(lang == NULL) ? "_NORMAL_" : gtk_source_language_get_id (lang),
			NULL);
	}

	doc->priv->language_set_by_user = set_by_user;
}

static void
set_encoding (PlumaDocument       *doc,
	      const PlumaEncoding *encoding,
	      gboolean             set_by_user)
{
	g_return_if_fail (encoding != NULL);

	pluma_debug (DEBUG_DOCUMENT);

	if (doc->priv->encoding == encoding)
		return;

	doc->priv->encoding = encoding;

	if (set_by_user)
	{
		const gchar *charset;

		charset = pluma_encoding_get_charset (encoding);

		pluma_document_set_metadata (doc, PLUMA_METADATA_ATTRIBUTE_ENCODING,
					     charset, NULL);
	}

	g_object_notify (G_OBJECT (doc), "encoding");
}

static GtkSourceStyleScheme *
get_default_style_scheme (GSettings *editor_settings)
{
	gchar *scheme_id;
	GtkSourceStyleScheme *def_style;
	GtkSourceStyleSchemeManager *manager;

	manager = pluma_get_style_scheme_manager ();
	scheme_id = g_settings_get_string (editor_settings, PLUMA_SETTINGS_COLOR_SCHEME);
	def_style = gtk_source_style_scheme_manager_get_scheme (manager,
								scheme_id);

	if (def_style == NULL)
	{
		g_warning ("Default style scheme '%s' cannot be found, falling back to 'classic' style scheme ", scheme_id);

		def_style = gtk_source_style_scheme_manager_get_scheme (manager, "classic");
		if (def_style == NULL)
		{
			g_warning ("Style scheme 'classic' cannot be found, check your GtkSourceView installation.");
		}
	}

	g_free (scheme_id);

	return def_style;
}

static void
on_uri_changed (PlumaDocument *doc,
		GParamSpec    *pspec,
		gpointer       useless)
{
#ifdef ENABLE_GVFS_METADATA
	GFile *location;

	location = pluma_document_get_location (doc);

	/* load metadata for this uri: we load sync since metadata is
	 * always local so it should be fast and we need the information
	 * right after the uri was set.
	 */
	if (location != NULL)
	{
		GError *error = NULL;

		if (doc->priv->metadata_info != NULL)
			g_object_unref (doc->priv->metadata_info);

		doc->priv->metadata_info = g_file_query_info (location,
							      METADATA_QUERY,
							      G_FILE_QUERY_INFO_NONE,
							      NULL,
							      &error);

		if (error != NULL)
		{
			if (error->code != G_FILE_ERROR_ISDIR &&
			    error->code != G_FILE_ERROR_NOTDIR &&
			    error->code != G_FILE_ERROR_NOENT)
			{
				g_warning ("%s", error->message);
			}

			g_error_free (error);
		}

		g_object_unref (location);
	}
#endif
}

static GtkSourceLanguage *
guess_language (PlumaDocument *doc,
		const gchar   *content_type)
{
	gchar *data;
	GtkSourceLanguage *language = NULL;

	data = pluma_document_get_metadata (doc, PLUMA_METADATA_ATTRIBUTE_LANGUAGE);

	if (data != NULL)
	{
		pluma_debug_message (DEBUG_DOCUMENT, "Language from metadata: %s", data);

		if (strcmp (data, "_NORMAL_") != 0)
		{
			language = gtk_source_language_manager_get_language (
						pluma_get_language_manager (),
						data);
		}

		g_free (data);
	}
	else
	{
		GFile *file;
		gchar *basename = NULL;

		file = pluma_document_get_location (doc);
		pluma_debug_message (DEBUG_DOCUMENT, "Sniffing Language");

		if (file)
		{
			basename = g_file_get_basename (file);
		}
		else if (doc->priv->short_name != NULL)
		{
			basename = g_strdup (doc->priv->short_name);
		}

		language = gtk_source_language_manager_guess_language (
					pluma_get_language_manager (),
					basename,
					content_type);

		g_free (basename);

		if (file != NULL)
		{
			g_object_unref (file);
		}
	}

	return language;
}

static void
on_content_type_changed (PlumaDocument *doc,
			 GParamSpec    *pspec,
			 gpointer       useless)
{
	if (!doc->priv->language_set_by_user)
	{
		GtkSourceLanguage *language;

		language = guess_language (doc, doc->priv->content_type);

		pluma_debug_message (DEBUG_DOCUMENT, "Language: %s",
				     language != NULL ? gtk_source_language_get_name (language) : "None");

		set_language (doc, language, FALSE);
	}
}

static gchar *
get_default_content_type (void)
{
	return g_content_type_from_mime_type ("text/plain");
}

static void
pluma_document_init (PlumaDocument *doc)
{
	GtkSourceStyleScheme *style_scheme;
	gint undo_actions;
	gboolean bracket_matching;
	gboolean search_hl;

	pluma_debug (DEBUG_DOCUMENT);

	doc->priv = pluma_document_get_instance_private (doc);

	doc->priv->editor_settings = g_settings_new (PLUMA_SCHEMA_ID);

	doc->priv->uri = NULL;
	doc->priv->untitled_number = get_untitled_number ();

	doc->priv->metadata_info = NULL;

	doc->priv->content_type = get_default_content_type ();

	doc->priv->readonly = FALSE;

	doc->priv->stop_cursor_moved_emission = FALSE;

	doc->priv->last_save_was_manually = TRUE;
	doc->priv->language_set_by_user = FALSE;

	doc->priv->dispose_has_run = FALSE;

	doc->priv->mtime = 0;

	doc->priv->time_of_last_save_or_load = g_get_real_time ();

	doc->priv->encoding = pluma_encoding_get_utf8 ();

	doc->priv->newline_type = PLUMA_DOCUMENT_NEWLINE_TYPE_DEFAULT;
	doc->priv->hide_trailing_newline = g_settings_get_boolean (doc->priv->editor_settings,
	                                                           PLUMA_SETTINGS_HIDE_TRAILING_NEWLINE);

	undo_actions = g_settings_get_uint (doc->priv->editor_settings, PLUMA_SETTINGS_MAX_UNDO_ACTIONS);

	bracket_matching = g_settings_get_boolean (doc->priv->editor_settings,
						   PLUMA_SETTINGS_BRACKET_MATCHING);
	search_hl = g_settings_get_boolean (doc->priv->editor_settings,
					    PLUMA_SETTINGS_SEARCH_HIGHLIGHTING);

	gtk_source_buffer_set_max_undo_levels (GTK_SOURCE_BUFFER (doc),
					       undo_actions);

	gtk_source_buffer_set_highlight_matching_brackets (GTK_SOURCE_BUFFER (doc),
							   bracket_matching);

	pluma_document_set_enable_search_highlighting (doc, search_hl);


	style_scheme = get_default_style_scheme (doc->priv->editor_settings);
	if (style_scheme != NULL)
		gtk_source_buffer_set_style_scheme (GTK_SOURCE_BUFFER (doc),
						    style_scheme);

	g_signal_connect_after (doc,
			  	"insert-text",
			  	G_CALLBACK (insert_text_cb),
			  	NULL);

	g_signal_connect_after (doc,
			  	"delete-range",
			  	G_CALLBACK (delete_range_cb),
			  	NULL);

	g_signal_connect (doc,
			  "notify::content-type",
			  G_CALLBACK (on_content_type_changed),
			  NULL);

	g_signal_connect (doc,
			  "notify::uri",
			  G_CALLBACK (on_uri_changed),
			  NULL);
}

PlumaDocument *
pluma_document_new (void)
{
	pluma_debug (DEBUG_DOCUMENT);

	return PLUMA_DOCUMENT (g_object_new (PLUMA_TYPE_DOCUMENT, NULL));
}

static void
set_content_type_no_guess (PlumaDocument *doc,
			   const gchar   *content_type)
{
	pluma_debug (DEBUG_DOCUMENT);

	if (doc->priv->content_type != NULL && content_type != NULL &&
	    (0 == strcmp (doc->priv->content_type, content_type)))
		return;

	g_free (doc->priv->content_type);

	if (content_type == NULL || g_content_type_is_unknown (content_type))
		doc->priv->content_type = get_default_content_type ();
	else
		doc->priv->content_type = g_strdup (content_type);

	g_object_notify (G_OBJECT (doc), "content-type");
}

static void
set_content_type (PlumaDocument *doc,
		  const gchar   *content_type)
{
	pluma_debug (DEBUG_DOCUMENT);

	if (content_type == NULL)
	{
		GFile *file;
		gchar *guessed_type = NULL;

		/* If content type is null, we guess from the filename */
		file = pluma_document_get_location (doc);
		if (file != NULL)
		{
			gchar *basename;

			basename = g_file_get_basename (file);
			guessed_type = g_content_type_guess (basename, NULL, 0, NULL);

			g_free (basename);
			g_object_unref (file);
		}

		set_content_type_no_guess (doc, guessed_type);

		g_free (guessed_type);
	}
	else
	{
		set_content_type_no_guess (doc, content_type);
	}
}

/**
 * pluma_document_set_content_type:
 * @doc:
 * @content_type: (allow-none):
 */
void
pluma_document_set_content_type (PlumaDocument *doc,
                                 const gchar   *content_type)
{
	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));

	set_content_type (doc, content_type);
}

static void
set_uri (PlumaDocument *doc,
	 const gchar   *uri)
{
	pluma_debug (DEBUG_DOCUMENT);

	g_return_if_fail ((uri == NULL) || pluma_utils_is_valid_uri (uri));

	if (uri != NULL)
	{
		if (doc->priv->uri == uri)
			return;

		g_free (doc->priv->uri);
		doc->priv->uri = g_strdup (uri);

		if (doc->priv->untitled_number > 0)
		{
			release_untitled_number (doc->priv->untitled_number);
			doc->priv->untitled_number = 0;
		}
	}

	g_object_notify (G_OBJECT (doc), "uri");

	if (doc->priv->short_name == NULL)
	{
		g_object_notify (G_OBJECT (doc), "shortname");
	}
}


/**
 * pluma_document_get_location:
 * @doc: a #PlumaDocument
 *
 * Returns: (allow-none) (transfer full): a new #GFile
 */
GFile *
pluma_document_get_location (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), NULL);

	return doc->priv->uri == NULL ? NULL : g_file_new_for_uri (doc->priv->uri);
}

gchar *
pluma_document_get_uri (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), NULL);

	return g_strdup (doc->priv->uri);
}

void
pluma_document_set_uri (PlumaDocument *doc,
			const gchar   *uri)
{
	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));
	g_return_if_fail (uri != NULL);

	set_uri (doc, uri);
	set_content_type (doc, NULL);
}

/**
 * pluma_document_get_uri_for_display:
 * @doc:
 *
 * Note: this never returns %NULL.
 **/
gchar *
pluma_document_get_uri_for_display (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), g_strdup (""));

	if (doc->priv->uri == NULL)
		return g_strdup_printf (_("Unsaved Document %d"),
					doc->priv->untitled_number);
	else
		return pluma_utils_uri_for_display (doc->priv->uri);
}

/**
 * pluma_document_get_short_name_for_display:
 * @doc:
 *
 * Note: this never returns %NULL.
 **/
gchar *
pluma_document_get_short_name_for_display (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), g_strdup (""));

	if (doc->priv->short_name != NULL)
		return g_strdup (doc->priv->short_name);
	else if (doc->priv->uri == NULL)
		return g_strdup_printf (_("Unsaved Document %d"),
					doc->priv->untitled_number);
	else
		return pluma_utils_basename_for_display (doc->priv->uri);
}

/**
 * pluma_document_set_short_name_for_display:
 * @doc:
 * @name: (allow-none):
 */
void
pluma_document_set_short_name_for_display (PlumaDocument *doc,
                                           const gchar   *short_name)
{
	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));

	g_free (doc->priv->short_name);
	doc->priv->short_name = g_strdup (short_name);

	g_object_notify (G_OBJECT (doc), "shortname");
}

gchar *
pluma_document_get_content_type (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), NULL);

 	return g_strdup (doc->priv->content_type);
}

/**
 * pluma_document_get_mime_type:
 * @doc:
 *
 * Note: this never returns %NULL.
 **/
gchar *
pluma_document_get_mime_type (PlumaDocument *doc)
{
	gchar *mime_type = NULL;

	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), g_strdup ("text/plain"));

	if ((doc->priv->content_type != NULL) &&
	    (!g_content_type_is_unknown (doc->priv->content_type)))
	{
		mime_type = g_content_type_get_mime_type (doc->priv->content_type);
	}

 	return mime_type != NULL ? mime_type : g_strdup ("text/plain");
}

/* Note: do not emit the notify::read-only signal */
static gboolean
set_readonly (PlumaDocument *doc,
	      gboolean       readonly)
{
	pluma_debug (DEBUG_DOCUMENT);

	readonly = (readonly != FALSE);

	if (doc->priv->readonly == readonly)
		return FALSE;

	doc->priv->readonly = readonly;

	return TRUE;
}

/**
 * pluma_document_set_readonly:
 * @doc: a #PlumaDocument
 * @readonly: %TRUE to se the document as read-only
 *
 * If @readonly is %TRUE sets @doc as read-only.
 */
void
_pluma_document_set_readonly (PlumaDocument *doc,
			      gboolean       readonly)
{
	pluma_debug (DEBUG_DOCUMENT);

	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));

	if (set_readonly (doc, readonly))
	{
		g_object_notify (G_OBJECT (doc), "read-only");
	}
}

gboolean
pluma_document_get_readonly (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), TRUE);

	return doc->priv->readonly;
}

gboolean
_pluma_document_check_externally_modified (PlumaDocument *doc)
{
	GFile *gfile;
	GFileInfo *info;

	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), FALSE);

	if (doc->priv->uri == NULL)
	{
		return FALSE;
	}

	gfile = g_file_new_for_uri (doc->priv->uri);
	info = g_file_query_info (gfile,
	                          G_FILE_ATTRIBUTE_TIME_MODIFIED "," \
	                          G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC "," \
	                          G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
	                          G_FILE_QUERY_INFO_NONE,
	                          NULL, NULL);
	g_object_unref (gfile);

	if (info != NULL)
	{
		/* While at it also check if permissions changed */
		if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
		{
			gboolean read_only;

			read_only = !g_file_info_get_attribute_boolean (info,
									G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);

			_pluma_document_set_readonly (doc, read_only);
		}

		if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
		{
			guint64 timeval;

			timeval = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED) * G_USEC_PER_SEC;
			if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC))
			{
				guint32 usec;

				usec = g_file_info_get_attribute_uint32 (info,
				                                         G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
				timeval += (guint64) usec;
			}
			g_object_unref (info);

			return (((gint64) timeval) > doc->priv->mtime);
		}
	}

	return FALSE;
}

static void
reset_temp_loading_data (PlumaDocument       *doc)
{
	/* the loader has been used, throw it away */
	g_object_unref (doc->priv->loader);
	doc->priv->loader = NULL;

	doc->priv->requested_encoding = NULL;
	doc->priv->requested_line_pos = 0;
}

static void
document_loader_loaded (PlumaDocumentLoader *loader,
			const GError        *error,
			PlumaDocument       *doc)
{
	/* load was successful */
	if (error == NULL ||
	    (error->domain == PLUMA_DOCUMENT_ERROR &&
	     error->code == PLUMA_DOCUMENT_ERROR_CONVERSION_FALLBACK))
	{
		GtkTextIter iter;
		GFileInfo *info;
		gboolean restore_cursor;
		const gchar *content_type = NULL;
		gboolean read_only = FALSE;
		guint64 mtime = 0;

		info = pluma_document_loader_get_info (loader);

		if (info)
		{
			if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE))
				content_type = g_file_info_get_attribute_string (info,
										 G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);

			if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
				mtime = g_file_info_get_attribute_uint64 (info,
				                                          G_FILE_ATTRIBUTE_TIME_MODIFIED) * G_USEC_PER_SEC;

			if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC))
			{
				guint32 usec;

				usec = g_file_info_get_attribute_uint32 (info,
				                                         G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
				mtime += (guint64) usec;
			}

			if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
				read_only = !g_file_info_get_attribute_boolean (info,
										G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
		}

		doc->priv->mtime = (gint64) mtime;

		set_readonly (doc, read_only);

		doc->priv->time_of_last_save_or_load = g_get_real_time ();

		set_encoding (doc,
			      pluma_document_loader_get_encoding (loader),
			      (doc->priv->requested_encoding != NULL));

		set_content_type (doc, content_type);

		pluma_document_set_newline_type (doc,
		                                 pluma_document_loader_get_newline_type (loader));

		if (doc->priv->hide_trailing_newline)
		{
			gboolean trimmed_trailing_newline;
			gint doc_char_count;

			g_object_get (loader,
			              "trimmed-trailing-newline", &trimmed_trailing_newline,
			              NULL);

			doc_char_count = gtk_text_buffer_get_char_count (GTK_TEXT_BUFFER (doc));

			if (!trimmed_trailing_newline && doc_char_count > 0)
			{
				/* Document did not contain any trailing newline, so we want to
				   change hide-trailing-newline to FALSE so that saving the
				   document doesn’t automatically add a trailing newline if it
				   wasn’t previously present. */
				/* Note that we special-case empty documents here as these never
				   contain a trailing newline, so we cannot make any assumptions
				   on whether the omission of the trailing newline was intentional
				   or not. */
				g_object_set (doc,
				              "hide-trailing-newline", FALSE,
				              NULL);
			}
		}

		restore_cursor = g_settings_get_boolean (doc->priv->editor_settings,
							 PLUMA_SETTINGS_RESTORE_CURSOR_POSITION);

		/* move the cursor at the requested line if any */
		if (doc->priv->requested_line_pos > 0)
		{
			/* line_pos - 1 because get_iter_at_line counts from 0 */
			gtk_text_buffer_get_iter_at_line (GTK_TEXT_BUFFER (doc),
							  &iter,
							  doc->priv->requested_line_pos - 1);
		}
		/* else, if enabled, to the position stored in the metadata */
		else if (restore_cursor)
		{
			gchar *pos;
			gint offset;

			pos = pluma_document_get_metadata (doc, PLUMA_METADATA_ATTRIBUTE_POSITION);

			offset = pos ? atoi (pos) : 0;
			g_free (pos);

			gtk_text_buffer_get_iter_at_offset (GTK_TEXT_BUFFER (doc),
							    &iter,
							    MAX (offset, 0));

			/* make sure it's a valid position, if the file
			 * changed we may have ended up in the middle of
			 * a utf8 character cluster */
			if (!gtk_text_iter_is_cursor_position (&iter))
			{
				gtk_text_buffer_get_start_iter (GTK_TEXT_BUFFER (doc),
								&iter);
			}
		}
		/* otherwise to the top */
		else
		{
			gtk_text_buffer_get_start_iter (GTK_TEXT_BUFFER (doc),
							&iter);
		}

		gtk_text_buffer_place_cursor (GTK_TEXT_BUFFER (doc), &iter);
	}

	/* special case creating a named new doc */
	else if (doc->priv->create &&
	         (error->domain == G_IO_ERROR && error->code == G_IO_ERROR_NOT_FOUND) &&
	         (pluma_utils_uri_has_file_scheme (doc->priv->uri)))
	{
		reset_temp_loading_data (doc);

		g_signal_emit (doc,
			       document_signals[LOADED],
			       0,
			       NULL);

		return;
	}

	g_signal_emit (doc,
		       document_signals[LOADED],
		       0,
		       error);

	reset_temp_loading_data (doc);
}

static void
document_loader_loading (PlumaDocumentLoader *loader,
			 gboolean             completed,
			 const GError        *error,
			 PlumaDocument       *doc)
{
	if (completed)
	{
		document_loader_loaded (loader, error, doc);
	}
	else
	{
		goffset size = 0;
		goffset read;
		GFileInfo *info;

		info = pluma_document_loader_get_info (loader);

		if (info && g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
			size = g_file_info_get_attribute_uint64 (info,
								 G_FILE_ATTRIBUTE_STANDARD_SIZE);

		read = pluma_document_loader_get_bytes_read (loader);

		g_signal_emit (doc,
			       document_signals[LOADING],
			       0,
			       read,
			       size);
	}
}

static void
pluma_document_load_real (PlumaDocument       *doc,
			  const gchar         *uri,
			  const PlumaEncoding *encoding,
			  gint                 line_pos,
			  gboolean             create)
{
	g_return_if_fail (doc->priv->loader == NULL);

	pluma_debug_message (DEBUG_DOCUMENT, "load_real: uri = %s", uri);

	/* create a loader. It will be destroyed when loading is completed */
	doc->priv->loader = pluma_document_loader_new (doc, uri, encoding);

	g_signal_connect (doc->priv->loader,
			  "loading",
			  G_CALLBACK (document_loader_loading),
			  doc);

	g_object_set (G_OBJECT (doc->priv->loader),
	              "trim-trailing-newline", doc->priv->hide_trailing_newline,
	              NULL);

	doc->priv->create = create;
	doc->priv->requested_encoding = encoding;
	doc->priv->requested_line_pos = line_pos;

	set_uri (doc, uri);
	set_content_type (doc, NULL);

	pluma_document_loader_load (doc->priv->loader);
}

/**
 * pluma_document_load:
 * @doc: the #PlumaDocument.
 * @uri: the uri where to load the document from.
 * @encoding: the #PlumaEncoding to encode the document.
 * @line_pos: the line to show.
 * @create: whether the document should be created if it doesn't exist.
 *
 * Load a document. This results in the "load" signal to be emitted.
 */
void
pluma_document_load (PlumaDocument       *doc,
		     const gchar         *uri,
		     const PlumaEncoding *encoding,
		     gint                 line_pos,
		     gboolean             create)
{
	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));
	g_return_if_fail (uri != NULL);
	g_return_if_fail (pluma_utils_is_valid_uri (uri));

	g_signal_emit (doc, document_signals[LOAD], 0, uri, encoding, line_pos, create);
}

/**
 * pluma_document_load_cancel:
 * @doc: the #PlumaDocument.
 *
 * Cancel load of a document.
 */
gboolean
pluma_document_load_cancel (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), FALSE);

	if (doc->priv->loader == NULL)
		return FALSE;

	return pluma_document_loader_cancel (doc->priv->loader);
}

static void
document_saver_saving (PlumaDocumentSaver *saver,
		       gboolean            completed,
		       const GError       *error,
		       PlumaDocument      *doc)
{
	pluma_debug (DEBUG_DOCUMENT);

	if (completed)
	{
		/* save was successful */
		if (error == NULL)
		{
			const gchar *uri;
			const gchar *content_type = NULL;
			guint64 mtime = 0;
			GFileInfo *info;

			uri = pluma_document_saver_get_uri (saver);
			set_uri (doc, uri);

			info = pluma_document_saver_get_info (saver);

			if (info != NULL)
			{
				if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE))
					content_type = g_file_info_get_attribute_string (info,
											 G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);

				if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
					mtime = g_file_info_get_attribute_uint64 (info,
					                                          G_FILE_ATTRIBUTE_TIME_MODIFIED) * G_USEC_PER_SEC;

				if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC))
				{
					guint32 usec;

					usec = g_file_info_get_attribute_uint32 (info,
					                                         G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
					mtime += (guint64) usec;
				}
			}

			set_content_type (doc, content_type);
			doc->priv->mtime = (gint64) mtime;

			doc->priv->time_of_last_save_or_load = g_get_real_time ();

			_pluma_document_set_readonly (doc, FALSE);

			gtk_text_buffer_set_modified (GTK_TEXT_BUFFER (doc),
						      FALSE);

			set_encoding (doc,
				      doc->priv->requested_encoding,
				      TRUE);
		}

		g_signal_emit (doc,
			       document_signals[SAVED],
			       0,
			       error);

		/* the saver has been used, throw it away */
		g_object_unref (doc->priv->saver);
		doc->priv->saver = NULL;
	}
	else
	{
		goffset size = 0;
		goffset written = 0;

		size = pluma_document_saver_get_file_size (saver);
		written = pluma_document_saver_get_bytes_written (saver);

		pluma_debug_message (DEBUG_DOCUMENT, "save progress: %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT, written, size);

		g_signal_emit (doc,
			       document_signals[SAVING],
			       0,
			       written,
			       size);
	}
}

static void
pluma_document_save_real (PlumaDocument          *doc,
			  const gchar            *uri,
			  const PlumaEncoding    *encoding,
			  PlumaDocumentSaveFlags  flags)
{
	g_return_if_fail (doc->priv->saver == NULL);

	/* create a saver, it will be destroyed once saving is complete */
	doc->priv->saver = pluma_document_saver_new (doc, uri, encoding,
						     doc->priv->newline_type,
						     flags);

	g_signal_connect (doc->priv->saver,
			  "saving",
			  G_CALLBACK (document_saver_saving),
			  doc);

	g_object_set (G_OBJECT (doc->priv->saver),
	              "add-trailing-newline", doc->priv->hide_trailing_newline,
	              NULL);

	doc->priv->requested_encoding = encoding;

	pluma_document_saver_save (doc->priv->saver,
				   &doc->priv->mtime);
}

/**
 * pluma_document_save:
 * @doc: the #PlumaDocument.
 * @flags: optionnal #PlumaDocumentSaveFlags.
 *
 * Save the document to its previous location. This results in the "save"
 * signal to be emitted.
 */
void
pluma_document_save (PlumaDocument          *doc,
		     PlumaDocumentSaveFlags  flags)
{
	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));
	g_return_if_fail (doc->priv->uri != NULL);

	g_signal_emit (doc,
		       document_signals[SAVE],
		       0,
		       doc->priv->uri,
		       doc->priv->encoding,
		       flags);
}

/**
 * pluma_document_save_as:
 * @doc: the #PlumaDocument.
 * @uri: the uri where to save the document.
 * @encoding: the #PlumaEncoding to encode the document.
 * @flags: optionnal #PlumaDocumentSaveFlags.
 *
 * Save the document to a new location. This results in the "save" signal
 * to be emitted.
 */
void
pluma_document_save_as (PlumaDocument          *doc,
			const gchar            *uri,
			const PlumaEncoding    *encoding,
			PlumaDocumentSaveFlags  flags)
{
	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));
	g_return_if_fail (uri != NULL);
	g_return_if_fail (encoding != NULL);

	/* priv->mtime refers to the the old uri (if any). Thus, it should be
	 * ignored when saving as. */
	g_signal_emit (doc,
		       document_signals[SAVE],
		       0,
		       uri,
		       encoding,
		       flags | PLUMA_DOCUMENT_SAVE_IGNORE_MTIME);
}

gboolean
pluma_document_insert_file (PlumaDocument       *doc,
			    GtkTextIter         *iter,
			    const gchar         *uri,
			    const PlumaEncoding *encoding)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), FALSE);
	g_return_val_if_fail (iter != NULL, FALSE);
	g_return_val_if_fail (gtk_text_iter_get_buffer (iter) ==
				GTK_TEXT_BUFFER (doc), FALSE);

	/* TODO */

	return FALSE;
}

gboolean
pluma_document_is_untouched (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), TRUE);

	return (doc->priv->uri == NULL) &&
		(!gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (doc)));
}

gboolean
pluma_document_is_untitled (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), TRUE);

	return (doc->priv->uri == NULL);
}

gboolean
pluma_document_is_local (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), FALSE);

	if (doc->priv->uri == NULL)
	{
		return FALSE;
	}

	return pluma_utils_uri_has_file_scheme (doc->priv->uri);
}

gboolean
pluma_document_get_deleted (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), FALSE);

	return doc->priv->uri && !pluma_utils_uri_exists (doc->priv->uri);
}

/*
 * If @line is bigger than the lines of the document, the cursor is moved
 * to the last line and FALSE is returned.
 */
gboolean
pluma_document_goto_line (PlumaDocument *doc,
			  gint           line)
{
	gboolean ret = TRUE;
	guint line_count;
	GtkTextIter iter;

	pluma_debug (DEBUG_DOCUMENT);

	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), FALSE);
	g_return_val_if_fail (line >= -1, FALSE);

	line_count = gtk_text_buffer_get_line_count (GTK_TEXT_BUFFER (doc));

	if (line >= line_count)
	{
		ret = FALSE;
		gtk_text_buffer_get_end_iter (GTK_TEXT_BUFFER (doc),
					      &iter);
	}
	else
	{
		gtk_text_buffer_get_iter_at_line (GTK_TEXT_BUFFER (doc),
						  &iter,
						  line);
	}

	gtk_text_buffer_place_cursor (GTK_TEXT_BUFFER (doc), &iter);

	return ret;
}

gboolean
pluma_document_goto_line_offset (PlumaDocument *doc,
				 gint           line,
				 gint           line_offset)
{
	gboolean ret = TRUE;
	guint offset_count;
	GtkTextIter iter;

	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), FALSE);
	g_return_val_if_fail (line >= -1, FALSE);
	g_return_val_if_fail (line_offset >= -1, FALSE);

	gtk_text_buffer_get_iter_at_line (GTK_TEXT_BUFFER (doc),
					  &iter,
					  line);

	offset_count = gtk_text_iter_get_chars_in_line (&iter);
	if (line_offset > offset_count)
	{
		ret = FALSE;
	}
	else
	{
		gtk_text_iter_set_line_offset (&iter, line_offset);
	}

	gtk_text_buffer_place_cursor (GTK_TEXT_BUFFER (doc), &iter);

	return ret;
}

static gint
compute_num_of_lines (const gchar *text)
{
	const gchar *p;
	gint len;
	gint n = 1;

	g_return_val_if_fail (text != NULL, 0);

	len = strlen (text);
	p = text;

	while (len > 0)
	{
		gint del, par;

		pango_find_paragraph_boundary (p, len, &del, &par);

		if (del == par) /* not found */
			break;

		p += par;
		len -= par;
		++n;
	}

	return n;
}

/**
 * pluma_document_set_search_text:
 * @doc:
 * @text: (allow-none):
 * @flags:
 **/
void
pluma_document_set_search_text (PlumaDocument *doc,
				const gchar   *text,
				guint          flags)
{
	gchar *converted_text;
	gboolean notify = FALSE;
	gboolean update_to_search_region = FALSE;

	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));
	g_return_if_fail ((text == NULL) || (doc->priv->search_text != text));
	g_return_if_fail ((text == NULL) || g_utf8_validate (text, -1, NULL));

	pluma_debug_message (DEBUG_DOCUMENT, "text = %s", text);

	if (text != NULL)
	{
		if (*text != '\0')
		{
			converted_text = pluma_utils_unescape_search_text (text);
			notify = !pluma_document_get_can_search_again (doc);
		}
		else
		{
			converted_text = g_strdup("");
			notify = pluma_document_get_can_search_again (doc);
		}

		g_free (doc->priv->search_text);

		doc->priv->search_text = converted_text;
		doc->priv->num_of_lines_search_text = compute_num_of_lines (doc->priv->search_text);
		update_to_search_region = TRUE;
	}

	if (!PLUMA_SEARCH_IS_DONT_SET_FLAGS (flags))
	{
		if (doc->priv->search_flags != flags)
			update_to_search_region = TRUE;

		doc->priv->search_flags = flags;

	}

	if (update_to_search_region)
	{
		GtkTextIter begin;
		GtkTextIter end;

		gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (doc),
					    &begin,
					    &end);

		to_search_region_range (doc,
					&begin,
					&end);
	}

	if (notify)
		g_object_notify (G_OBJECT (doc), "can-search-again");
}

/**
 * pluma_document_get_search_text:
 * @doc:
 * @flags: (allow-none):
 */
gchar *
pluma_document_get_search_text (PlumaDocument *doc,
				guint         *flags)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), NULL);

	if (flags != NULL)
		*flags = doc->priv->search_flags;

	return pluma_utils_escape_search_text (doc->priv->search_text);
}

/**
 * pluma_document_set_last_replace_text:
 * @doc:
 * @text: (allow-none):
 **/
void
pluma_document_set_last_replace_text (PlumaDocument *doc,
				      const gchar   *text)
{
	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));

	g_free(doc->priv->last_replace_text);

	pluma_debug_message (DEBUG_SEARCH, "last_replace_text = %s", text == NULL ? "NULL" : text);
	doc->priv->last_replace_text = g_strdup(text);
}

/**
 * pluma_document_get_last_replace_text:
 * @doc:
 */
gchar *
pluma_document_get_last_replace_text (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), NULL);

	return doc->priv->last_replace_text;
}

gboolean
pluma_document_get_can_search_again (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), FALSE);

	return ((doc->priv->search_text != NULL) &&
	        (*doc->priv->search_text != '\0'));
}

/**
 * pluma_document_search_forward:
 * @doc:
 * @start: (allow-none):
 * @end: (allow-none):
 * @match_start: (allow-none):
 * @match_end: (allow-none):
 **/
gboolean
pluma_document_search_forward (PlumaDocument     *doc,
			       const GtkTextIter *start,
			       const GtkTextIter *end,
			       GtkTextIter       *match_start,
			       GtkTextIter       *match_end)
{
	GtkTextIter iter;
	GtkTextSearchFlags search_flags;
	gboolean found = FALSE;
	GtkTextIter m_start;
	GtkTextIter m_end;

	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), FALSE);
	g_return_val_if_fail ((start == NULL) ||
			      (gtk_text_iter_get_buffer (start) ==  GTK_TEXT_BUFFER (doc)), FALSE);
	g_return_val_if_fail ((end == NULL) ||
			      (gtk_text_iter_get_buffer (end) ==  GTK_TEXT_BUFFER (doc)), FALSE);

	if (doc->priv->search_text == NULL)
	{
		pluma_debug_message (DEBUG_DOCUMENT, "doc->priv->search_text == NULL\n");
		return FALSE;
	}
	else
		pluma_debug_message (DEBUG_DOCUMENT, "doc->priv->search_text == \"%s\"\n", doc->priv->search_text);

	if (start == NULL)
		gtk_text_buffer_get_start_iter (GTK_TEXT_BUFFER (doc), &iter);
	else
		iter = *start;

	search_flags = GTK_TEXT_SEARCH_VISIBLE_ONLY | GTK_TEXT_SEARCH_TEXT_ONLY;

	if (!PLUMA_SEARCH_IS_CASE_SENSITIVE (doc->priv->search_flags))
	{
		search_flags = search_flags | GTK_TEXT_SEARCH_CASE_INSENSITIVE;
	}

	while (!found)
	{
		if(!PLUMA_SEARCH_IS_MATCH_REGEX(doc->priv->search_flags))
		{
			found = gtk_text_iter_forward_search (&iter,
							      doc->priv->search_text,
							      search_flags,
							      &m_start,
							      &m_end,
							      end);
		} else {
			found = pluma_gtk_text_iter_regex_search (&iter,
								  doc->priv->search_text,
								  search_flags,
								  &m_start,
								  &m_end,
								  end,
								  TRUE,
								  &doc->priv->last_replace_text);
		}

		if (found && PLUMA_SEARCH_IS_ENTIRE_WORD (doc->priv->search_flags))
		{
			found = gtk_text_iter_starts_word (&m_start) &&
					gtk_text_iter_ends_word (&m_end);

			if (!found)
				iter = m_end;
		}
		else
			break;
	}

	if (found && (match_start != NULL))
		*match_start = m_start;

	if (found && (match_end != NULL))
		*match_end = m_end;

	return found;
}

/**
 * pluma_document_search_backward:
 * @doc:
 * @start: (allow-none):
 * @end: (allow-none):
 * @match_start: (allow-none):
 * @match_end: (allow-none):
 **/
gboolean
pluma_document_search_backward (PlumaDocument     *doc,
		const GtkTextIter *start,
		const GtkTextIter *end,
		GtkTextIter       *match_start,
		GtkTextIter       *match_end)
{
	GtkTextIter iter;
	GtkTextSearchFlags search_flags;
	gboolean found = FALSE;
	GtkTextIter m_start;
	GtkTextIter m_end;

	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), FALSE);
	g_return_val_if_fail ((start == NULL) ||
(			      gtk_text_iter_get_buffer (start) ==  GTK_TEXT_BUFFER (doc)), FALSE);
	g_return_val_if_fail ((end == NULL) ||
			      (gtk_text_iter_get_buffer (end) ==  GTK_TEXT_BUFFER (doc)), FALSE);

	if (doc->priv->search_text == NULL)
	{
		pluma_debug_message (DEBUG_DOCUMENT, "doc->priv->search_text == NULL\n");
		return FALSE;
	}
	else
		pluma_debug_message (DEBUG_DOCUMENT, "doc->priv->search_text == \"%s\"\n", doc->priv->search_text);

	if (end == NULL)
		gtk_text_buffer_get_end_iter (GTK_TEXT_BUFFER (doc), &iter);
	else
		iter = *end;

	search_flags = GTK_TEXT_SEARCH_VISIBLE_ONLY | GTK_TEXT_SEARCH_TEXT_ONLY;

	if (!PLUMA_SEARCH_IS_CASE_SENSITIVE (doc->priv->search_flags))
	{
		search_flags = search_flags | GTK_TEXT_SEARCH_CASE_INSENSITIVE;
	}

	while (!found)
	{
		if(!PLUMA_SEARCH_IS_MATCH_REGEX(doc->priv->search_flags))
		{
			found = gtk_text_iter_backward_search (&iter,
							       doc->priv->search_text,
							       search_flags,
							       &m_start,
							       &m_end,
							       start);
		}
		else
		{
			found = pluma_gtk_text_iter_regex_search (&iter,
								  doc->priv->search_text,
								  search_flags,
								  &m_start,
								  &m_end,
								  start,
								  FALSE,
								  &doc->priv->last_replace_text);
		}

		if (found && PLUMA_SEARCH_IS_ENTIRE_WORD (doc->priv->search_flags))
		{
			found = gtk_text_iter_starts_word (&m_start) &&
			gtk_text_iter_ends_word (&m_end);

			if (!found)
				iter = m_start;
		}
		else
			break;
	}

	if (found && (match_start != NULL))
		*match_start = m_start;

	if (found && (match_end != NULL))
		*match_end = m_end;

	return found;
}

/* FIXME this is an issue for introspection regardning @find */
gint
pluma_document_replace_all (PlumaDocument       *doc,
			    const gchar         *find,
			    const gchar         *replace,
			    guint                flags)
{
	GtkTextIter iter;
	GtkTextIter m_start;
	GtkTextIter m_end;
	GtkTextSearchFlags search_flags = 0;
	gboolean found = TRUE;
	gint cont = 0;
	gchar *search_text;
	gchar *replace_text = NULL;
	gint replace_text_len = 0;
	GtkTextBuffer *buffer;
	gboolean brackets_highlighting;
	gboolean search_highliting;

	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), 0);
	g_return_val_if_fail (replace != NULL, 0);
	g_return_val_if_fail ((find != NULL) || (doc->priv->search_text != NULL), 0);

	buffer = GTK_TEXT_BUFFER (doc);

	if (find == NULL)
		search_text = g_strdup (doc->priv->search_text);
	else
		search_text = pluma_utils_unescape_search_text (find);

	if(!PLUMA_SEARCH_IS_MATCH_REGEX(flags))
	{
		replace_text = pluma_utils_unescape_search_text (replace);
		replace_text_len = strlen (replace_text);
	}

	gtk_text_buffer_get_start_iter (buffer, &iter);

	search_flags = GTK_TEXT_SEARCH_VISIBLE_ONLY | GTK_TEXT_SEARCH_TEXT_ONLY;

	if (!PLUMA_SEARCH_IS_CASE_SENSITIVE (flags))
	{
		search_flags = search_flags | GTK_TEXT_SEARCH_CASE_INSENSITIVE;
	}


	/* disable cursor_moved emission until the end of the
	 * replace_all so that we don't spend all the time
	 * updating the position in the statusbar
	 */
	doc->priv->stop_cursor_moved_emission = TRUE;

	/* also avoid spending time matching brackets */
	brackets_highlighting = gtk_source_buffer_get_highlight_matching_brackets (GTK_SOURCE_BUFFER (buffer));
	gtk_source_buffer_set_highlight_matching_brackets (GTK_SOURCE_BUFFER (buffer), FALSE);

	/* and do search highliting later */
	search_highliting = pluma_document_get_enable_search_highlighting (doc);
	pluma_document_set_enable_search_highlighting (doc, FALSE);

	gtk_text_buffer_begin_user_action (buffer);

	do
	{
		if(!PLUMA_SEARCH_IS_MATCH_REGEX(flags))
		{
			found = gtk_text_iter_forward_search (&iter,
							      search_text,
							      search_flags,
							      &m_start,
							      &m_end,
							      NULL);
		} else {
			g_free (replace_text);
			replace_text = g_strdup (replace);
			found = pluma_gtk_text_iter_regex_search (&iter,
							          search_text,
							          search_flags,
							          &m_start,
							          &m_end,
							          NULL,
							          TRUE,
							          &replace_text);
			replace_text_len = strlen (replace_text);
		}

		if (found && PLUMA_SEARCH_IS_ENTIRE_WORD (flags))
		{
			gboolean word;

			word = gtk_text_iter_starts_word (&m_start) &&
			       gtk_text_iter_ends_word (&m_end);

			if (!word)
			{
				iter = m_end;
				continue;
			}
		}

		if (found)
		{
			++cont;

			gtk_text_buffer_delete (buffer,
						&m_start,
						&m_end);
			gtk_text_buffer_insert (buffer,
						&m_start,
						replace_text,
						replace_text_len);

			iter = m_start;
        }

	} while (found);

	gtk_text_buffer_end_user_action (buffer);

	/* re-enable cursor_moved emission and notify
	 * the current position
	 */
	doc->priv->stop_cursor_moved_emission = FALSE;
	emit_cursor_moved (doc);

	gtk_source_buffer_set_highlight_matching_brackets (GTK_SOURCE_BUFFER (buffer),
							   brackets_highlighting);
	pluma_document_set_enable_search_highlighting (doc, search_highliting);

	g_free (search_text);
	g_free (replace_text);

	return cont;
}

/**
 * pluma_document_set_language:
 * @doc:
 * @lang: (allow-none):
 **/
void
pluma_document_set_language (PlumaDocument     *doc,
			     GtkSourceLanguage *lang)
{
	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));

	set_language (doc, lang, TRUE);
}

/**
 * pluma_document_get_language:
 * @doc:
 *
 * Return value: (transfer none):
 */
GtkSourceLanguage *
pluma_document_get_language (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), NULL);

	return gtk_source_buffer_get_language (GTK_SOURCE_BUFFER (doc));
}

const PlumaEncoding *
pluma_document_get_encoding (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), NULL);

	return doc->priv->encoding;
}

glong
_pluma_document_get_seconds_since_last_save_or_load (PlumaDocument *doc)
{
	pluma_debug (DEBUG_DOCUMENT);

	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), -1);

	return ((g_get_real_time () - doc->priv->time_of_last_save_or_load) / G_USEC_PER_SEC);
}

static void
get_search_match_colors (PlumaDocument *doc,
			 gboolean      *foreground_set,
			 GdkRGBA       *foreground,
			 gboolean      *background_set,
			 GdkRGBA       *background)
{
	GtkSourceStyleScheme *style_scheme;
	GtkSourceStyle *style;
	gchar *bg;
	gchar *fg;

	style_scheme = gtk_source_buffer_get_style_scheme (GTK_SOURCE_BUFFER (doc));
	if (style_scheme == NULL)
		goto fallback;

	style = gtk_source_style_scheme_get_style (style_scheme,
						   "search-match");
	if (style == NULL)
		goto fallback;

	g_object_get (style,
		      "foreground-set", foreground_set,
		      "foreground", &fg,
		      "background-set", background_set,
		      "background", &bg,
		      NULL);

	if (*foreground_set)
	{
		if (fg == NULL ||
		    !gdk_rgba_parse (foreground, fg))
		{
			*foreground_set = FALSE;
		}
	}

	if (*background_set)
	{
		if (bg == NULL ||
		    !gdk_rgba_parse (background, bg))
		{
			*background_set = FALSE;
		}
	}

	g_free (fg);
	g_free (bg);

	return;

 fallback:
	pluma_debug_message (DEBUG_DOCUMENT,
			     "Falling back to hard-coded colors "
			     "for the \"found\" text tag.");

	gdk_rgba_parse (background, "#FFFF78");
	*background_set = TRUE;
	*foreground_set = FALSE;

	return;
}

static void
sync_found_tag (PlumaDocument *doc,
		GParamSpec    *pspec,
		gpointer       data)
{
	GdkRGBA fg;
	GdkRGBA bg;
	gboolean fg_set;
	gboolean bg_set;

	pluma_debug (DEBUG_DOCUMENT);

	g_return_if_fail (GTK_TEXT_TAG (doc->priv->found_tag));

	get_search_match_colors (doc,
				 &fg_set, &fg,
				 &bg_set, &bg);

	g_object_set (doc->priv->found_tag,
		      "foreground-rgba", fg_set ? &fg : NULL,
		      NULL);
	g_object_set (doc->priv->found_tag,
		      "background-rgba", bg_set ? &bg : NULL,
		      NULL);
}

static void
text_tag_set_highest_priority (GtkTextTag    *tag,
			       GtkTextBuffer *buffer)
{
	GtkTextTagTable *table;
	gint n;

	table = gtk_text_buffer_get_tag_table (buffer);
	n = gtk_text_tag_table_get_size (table);
	gtk_text_tag_set_priority (tag, n - 1);
}

static void
search_region (PlumaDocument *doc,
	       GtkTextIter   *start,
	       GtkTextIter   *end)
{
	GtkTextIter iter;
	GtkTextIter m_start;
	GtkTextIter m_end;
	GtkTextSearchFlags search_flags = 0;
	gboolean found = TRUE;

	GtkTextBuffer *buffer;

	pluma_debug (DEBUG_DOCUMENT);

	buffer = GTK_TEXT_BUFFER (doc);

	if (doc->priv->found_tag == NULL)
	{
		doc->priv->found_tag = gtk_text_buffer_create_tag (GTK_TEXT_BUFFER (doc),
								   "found",
								   NULL);

		sync_found_tag (doc, NULL, NULL);

		g_signal_connect (doc,
				  "notify::style-scheme",
				  G_CALLBACK (sync_found_tag),
				  NULL);
	}

	/* make sure the 'found' tag has the priority over
	 * syntax highlighting tags */
	text_tag_set_highest_priority (doc->priv->found_tag,
				       GTK_TEXT_BUFFER (doc));


	if (doc->priv->search_text == NULL)
		return;

	g_return_if_fail (doc->priv->num_of_lines_search_text > 0);

	gtk_text_iter_backward_lines (start, doc->priv->num_of_lines_search_text);
	gtk_text_iter_forward_lines (end, doc->priv->num_of_lines_search_text);

	if (gtk_text_iter_has_tag (start, doc->priv->found_tag) &&
	    !gtk_text_iter_starts_tag (start, doc->priv->found_tag))
		gtk_text_iter_backward_to_tag_toggle (start, doc->priv->found_tag);

	if (gtk_text_iter_has_tag (end, doc->priv->found_tag) &&
	    !gtk_text_iter_ends_tag (end, doc->priv->found_tag))
		gtk_text_iter_forward_to_tag_toggle (end, doc->priv->found_tag);

	/*
	g_print ("[%u (%u), %u (%u)]\n", gtk_text_iter_get_line (start), gtk_text_iter_get_offset (start),
					   gtk_text_iter_get_line (end), gtk_text_iter_get_offset (end));
	*/

	gtk_text_buffer_remove_tag (buffer,
				    doc->priv->found_tag,
				    start,
				    end);

	if (*doc->priv->search_text == '\0')
		return;

	iter = *start;

	search_flags = GTK_TEXT_SEARCH_VISIBLE_ONLY | GTK_TEXT_SEARCH_TEXT_ONLY;

	if (!PLUMA_SEARCH_IS_CASE_SENSITIVE (doc->priv->search_flags))
	{
		search_flags = search_flags | GTK_TEXT_SEARCH_CASE_INSENSITIVE;
	}

	do
	{
		if ((end != NULL) && gtk_text_iter_is_end (end))
			end = NULL;

		found = gtk_text_iter_forward_search (&iter,
							doc->priv->search_text,
							search_flags,
                        	                	&m_start,
                        	                	&m_end,
                                	               	end);

		iter = m_end;

		if (found && PLUMA_SEARCH_IS_ENTIRE_WORD (doc->priv->search_flags))
		{
			gboolean word;

			word = gtk_text_iter_starts_word (&m_start) &&
			       gtk_text_iter_ends_word (&m_end);

			if (!word)
				continue;
		}

		if (found)
		{
			gtk_text_buffer_apply_tag (buffer,
						   doc->priv->found_tag,
						   &m_start,
						   &m_end);
		}

	} while (found);
}

static void
to_search_region_range (PlumaDocument *doc,
			GtkTextIter   *start,
			GtkTextIter   *end)
{
	pluma_debug (DEBUG_DOCUMENT);

	if (doc->priv->to_search_region == NULL)
		return;

	gtk_text_iter_set_line_offset (start, 0);
	gtk_text_iter_forward_to_line_end (end);

	/*
	g_print ("+ [%u (%u), %u (%u)]\n", gtk_text_iter_get_line (start), gtk_text_iter_get_offset (start),
					   gtk_text_iter_get_line (end), gtk_text_iter_get_offset (end));
	*/

	/* Add the region to the refresh region */
	pluma_text_region_add (doc->priv->to_search_region, start, end);

	/* Notify views of the updated highlight region */
	gtk_text_iter_backward_lines (start, doc->priv->num_of_lines_search_text);
	gtk_text_iter_forward_lines (end, doc->priv->num_of_lines_search_text);

	g_signal_emit (doc, document_signals [SEARCH_HIGHLIGHT_UPDATED], 0, start, end);
}

void
_pluma_document_search_region (PlumaDocument     *doc,
			       const GtkTextIter *start,
			       const GtkTextIter *end)
{
	PlumaTextRegion *region;

	pluma_debug (DEBUG_DOCUMENT);

	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));
	g_return_if_fail (start != NULL);
	g_return_if_fail (end != NULL);

	if (doc->priv->to_search_region == NULL)
		return;

	/*
	g_print ("U [%u (%u), %u (%u)]\n", gtk_text_iter_get_line (start), gtk_text_iter_get_offset (start),
					   gtk_text_iter_get_line (end), gtk_text_iter_get_offset (end));
	*/

	/* get the subregions not yet highlighted */
	region = pluma_text_region_intersect (doc->priv->to_search_region,
					      start,
					      end);
	if (region)
	{
		gint i;
		GtkTextIter start_search;
		GtkTextIter end_search;

		i = pluma_text_region_subregions (region);
		pluma_text_region_nth_subregion (region,
						 0,
						 &start_search,
						 NULL);

		pluma_text_region_nth_subregion (region,
						 i - 1,
						 NULL,
						 &end_search);

		pluma_text_region_destroy (region, TRUE);

		gtk_text_iter_order (&start_search, &end_search);

		search_region (doc, &start_search, &end_search);

		/* remove the just highlighted region */
		pluma_text_region_subtract (doc->priv->to_search_region,
					    start,
					    end);
	}
}

static void
insert_text_cb (PlumaDocument *doc,
		GtkTextIter   *pos,
		const gchar   *text,
		gint           length)
{
	GtkTextIter start;
	GtkTextIter end;

	pluma_debug (DEBUG_DOCUMENT);

	start = end = *pos;

	/*
	 * pos is invalidated when
	 * insertion occurs (because the buffer contents change), but the
	 * default signal handler revalidates it to point to the end of the
	 * inserted text
	 */
	gtk_text_iter_backward_chars (&start,
				      g_utf8_strlen (text, length));

	to_search_region_range (doc, &start, &end);
}

static void
delete_range_cb (PlumaDocument *doc,
		 GtkTextIter   *start,
		 GtkTextIter   *end)
{
	GtkTextIter d_start;
	GtkTextIter d_end;

	pluma_debug (DEBUG_DOCUMENT);

	d_start = *start;
	d_end = *end;

	to_search_region_range (doc, &d_start, &d_end);
}

void
pluma_document_set_enable_search_highlighting (PlumaDocument *doc,
					       gboolean       enable)
{
	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));

	enable = enable != FALSE;

	if ((doc->priv->to_search_region != NULL) == enable)
		return;

	if (doc->priv->to_search_region != NULL)
	{
		/* Disable search highlighting */
		if (doc->priv->found_tag != NULL)
		{
			/* If needed remove the found_tag */
			GtkTextIter begin;
			GtkTextIter end;

			gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (doc),
						    &begin,
						    &end);

			gtk_text_buffer_remove_tag (GTK_TEXT_BUFFER (doc),
				    		    doc->priv->found_tag,
				    		    &begin,
				    		    &end);
		}

		pluma_text_region_destroy (doc->priv->to_search_region,
					   TRUE);
		doc->priv->to_search_region = NULL;
	}
	else
	{
		doc->priv->to_search_region = pluma_text_region_new (GTK_TEXT_BUFFER (doc));
		if (pluma_document_get_can_search_again (doc))
		{
			/* If search_text is not empty, highligth all its occurrences */
			GtkTextIter begin;
			GtkTextIter end;

			gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (doc),
						    &begin,
						    &end);

			to_search_region_range (doc,
						&begin,
						&end);
		}
	}
}

gboolean
pluma_document_get_enable_search_highlighting (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), FALSE);

	return (doc->priv->to_search_region != NULL);
}

void
pluma_document_set_newline_type (PlumaDocument           *doc,
				 PlumaDocumentNewlineType newline_type)
{
	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));

	if (doc->priv->newline_type != newline_type)
	{
		doc->priv->newline_type = newline_type;

		g_object_notify (G_OBJECT (doc), "newline-type");
	}
}

PlumaDocumentNewlineType
pluma_document_get_newline_type (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), 0);

	return doc->priv->newline_type;
}

void
_pluma_document_set_mount_operation_factory (PlumaDocument 	       *doc,
					    PlumaMountOperationFactory	callback,
					    gpointer	                userdata)
{
	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));

	doc->priv->mount_operation_factory = callback;
	doc->priv->mount_operation_userdata = userdata;
}

GMountOperation *
_pluma_document_create_mount_operation (PlumaDocument *doc)
{
	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), NULL);

	if (doc->priv->mount_operation_factory == NULL)
		return g_mount_operation_new ();
	else
		return doc->priv->mount_operation_factory (doc,
						           doc->priv->mount_operation_userdata);
}

#ifndef ENABLE_GVFS_METADATA
gchar *
pluma_document_get_metadata (PlumaDocument *doc,
			     const gchar   *key)
{
	gchar *value = NULL;

	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), NULL);
	g_return_val_if_fail (key != NULL, NULL);

	if (!pluma_document_is_untitled (doc))
	{
		value = pluma_metadata_manager_get (doc->priv->uri, key);
	}

	return value;
}

void
pluma_document_set_metadata (PlumaDocument *doc,
			     const gchar   *first_key,
			     ...)
{
	const gchar *key;
	const gchar *value;
	va_list var_args;

	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));
	g_return_if_fail (first_key != NULL);

	if (pluma_document_is_untitled (doc))
	{
		/* Can't set metadata for untitled documents */
		return;
	}

	va_start (var_args, first_key);

	for (key = first_key; key; key = va_arg (var_args, const gchar *))
	{
		value = va_arg (var_args, const gchar *);

		pluma_metadata_manager_set (doc->priv->uri,
					    key,
					    value);
	}

	va_end (var_args);
}

#else

/**
 * pluma_document_get_metadata:
 * @doc: a #PlumaDocument
 * @key: name of the key
 *
 * Gets the metadata assigned to @key.
 *
 * Returns: the value assigned to @key.
 */
gchar *
pluma_document_get_metadata (PlumaDocument *doc,
			     const gchar   *key)
{
	gchar *value = NULL;

	g_return_val_if_fail (PLUMA_IS_DOCUMENT (doc), NULL);
	g_return_val_if_fail (key != NULL, NULL);

	if (doc->priv->metadata_info && g_file_info_has_attribute (doc->priv->metadata_info,
								   key))
	{
		value = g_strdup (g_file_info_get_attribute_string (doc->priv->metadata_info,
								    key));
	}

	return value;
}

static void
set_attributes_cb (GObject      *source,
		   GAsyncResult *res,
		   gpointer      useless)
{
	g_file_set_attributes_finish (G_FILE (source),
				      res,
				      NULL,
				      NULL);
}

/**
 * pluma_document_set_metadata:
 * @doc: a #PlumaDocument
 * @first_key: name of the first key to set
 * @...: value for the first key, followed optionally by more key/value pairs,
 * followed by %NULL.
 *
 * Sets metadata on a document.
 */
void
pluma_document_set_metadata (PlumaDocument *doc,
			     const gchar   *first_key,
			     ...)
{
	const gchar *key;
	const gchar *value;
	va_list var_args;
	GFileInfo *info;
	GFile *location;

	g_return_if_fail (PLUMA_IS_DOCUMENT (doc));
	g_return_if_fail (first_key != NULL);

	info = g_file_info_new ();

	va_start (var_args, first_key);

	for (key = first_key; key; key = va_arg (var_args, const gchar *))
	{
		value = va_arg (var_args, const gchar *);

		if (value != NULL)
		{
			g_file_info_set_attribute_string (info,
							  key, value);
		}
		else
		{
			/* Unset the key */
			g_file_info_set_attribute (info, key,
						   G_FILE_ATTRIBUTE_TYPE_INVALID,
						   NULL);
		}
	}

	va_end (var_args);

	if (doc->priv->metadata_info != NULL)
		g_file_info_copy_into (info, doc->priv->metadata_info);

	location = pluma_document_get_location (doc);

	if (location != NULL)
	{
		g_file_set_attributes_async (location,
					     info,
					     G_FILE_QUERY_INFO_NONE,
					     G_PRIORITY_DEFAULT,
					     NULL,
					     set_attributes_cb,
					     NULL);

		g_object_unref (location);
	}

	g_object_unref (info);
}
#endif
