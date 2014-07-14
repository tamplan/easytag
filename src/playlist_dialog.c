/* EasyTAG - tag editor for audio files
 * Copyright (C) 2013-2014  David King <amigadave@amigadave.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include "playlist_dialog.h"

#include <glib/gi18n.h>

#include "application_window.h"
#include "bar.h"
#include "browser.h"
#include "charset.h"
#include "easytag.h"
#include "gtk2_compat.h"
#include "log.h"
#include "misc.h"
#include "picture.h"
#include "scan.h"
#include "scan_dialog.h"
#include "setting.h"

/* TODO: Use G_DEFINE_TYPE_WITH_PRIVATE. */
G_DEFINE_TYPE (EtPlaylistDialog, et_playlist_dialog, GTK_TYPE_DIALOG)

#define et_playlist_dialog_get_instance_private(dialog) (dialog->priv)

static const guint BOX_SPACING = 6;

struct _EtPlaylistDialogPrivate
{
    GtkWidget *name_mask_entry;
    GtkWidget *content_mask_entry;
};

/*
 * Function to replace UNIX ForwardSlash with a DOS BackSlash
 */
static void
convert_forwardslash_to_backslash (const gchar *string)
{
    gchar *tmp;

    while ((tmp = strchr (string,'/')) != NULL)
    {
        *tmp = '\\';
    }
}

/*
 * Write a playlist
 *  - 'playlist_name' in file system encoding (not UTF-8)
 */
static gboolean
write_playlist (EtPlaylistDialog *self, GFile *file, GError **error)
{
    EtPlaylistDialogPrivate *priv;
    GFile *parent;
    GFileOutputStream *ostream;
    GString *to_write;
    ET_File *etfile;
    GList *l;
    GList *etfilelist = NULL;
    gchar *filename;
    gchar *basedir;
    gchar *temp;
    gint duration;
    EtPlaylistContent playlist_content;

    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    priv = et_playlist_dialog_get_instance_private (self);

    ostream = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);

    if (!ostream)
    {
        g_assert (error == NULL || *error != NULL);
        return FALSE;
    }

    /* 'base directory' where is located the playlist. Used also to write file with a
     * relative path for file located in this directory and sub-directories
     */
    parent = g_file_get_parent (file);
    basedir = g_file_get_path (parent);
    g_object_unref (parent);

    playlist_content = g_settings_get_enum (MainSettings, "playlist-content");

    /* 1) First line of the file (if playlist content is not set to "write only
     * list of files") */
    if (playlist_content != ET_PLAYLIST_CONTENT_FILENAMES)
    {
        gsize bytes_written;

        to_write = g_string_new ("#EXTM3U\r\n");

        if (!g_output_stream_write_all (G_OUTPUT_STREAM (ostream),
                                        to_write->str, to_write->len,
                                        &bytes_written, NULL, error))
        {
            g_debug ("Only %" G_GSIZE_FORMAT " bytes out of %" G_GSIZE_FORMAT
                     "bytes of data were written", bytes_written,
                     to_write->len);
            g_assert (error == NULL || *error != NULL);
            g_string_free (to_write, TRUE);
            g_object_unref (ostream);
            return FALSE;
        }
        g_string_free (to_write, TRUE);
    }

    if (g_settings_get_boolean (MainSettings, "playlist-selected-only"))
    {
        GList *selfilelist = NULL;
        GtkTreeSelection *selection = et_application_window_browser_get_selection (ET_APPLICATION_WINDOW (MainWindow));

        selfilelist = gtk_tree_selection_get_selected_rows(selection, NULL);

        for (l = selfilelist; l != NULL; l = g_list_next (l))
        {
            etfile = et_application_window_browser_get_et_file_from_path (ET_APPLICATION_WINDOW (MainWindow),
                                                                          l->data);
            etfilelist = g_list_prepend (etfilelist, etfile);
        }

        etfilelist = g_list_reverse (etfilelist);

        g_list_free_full (selfilelist, (GDestroyNotify)gtk_tree_path_free);
    }else
    {
        etfilelist = g_list_first(ETCore->ETFileList);
    }

    for (l = etfilelist; l != NULL; l = g_list_next (l))
    {
        etfile = (ET_File *)l->data;
        filename = ((File_Name *)etfile->FileNameCur->data)->value;
        duration = ((ET_File_Info *)etfile->ETFileInfo)->duration;

        if (g_settings_get_boolean (MainSettings, "playlist-relative"))
        {
            // Keep only files in this directory and sub-dirs
            if ( strncmp(filename,basedir,strlen(basedir))==0 )
            {
                gsize bytes_written;

                /* 2) Write the header. */
                switch (playlist_content)
                {
                    case ET_PLAYLIST_CONTENT_FILENAMES:
                        /* No header written. */
                        break;
                    case ET_PLAYLIST_CONTENT_EXTENDED:
                        /* Header has extended information. */
                        temp = g_path_get_basename (filename);
                        to_write = g_string_new ("#EXTINF:");
                        /* Must be written in system encoding (not UTF-8). */
                        g_string_append_printf (to_write, "%d,%s\r\n", duration,
                                                temp);

                        if (!g_output_stream_write_all (G_OUTPUT_STREAM (ostream),
                                                        to_write->str,
                                                        to_write->len,
                                                        &bytes_written, NULL,
                                                        error))
                        {
                            g_debug ("Only %" G_GSIZE_FORMAT " bytes out of %"
                                     G_GSIZE_FORMAT "bytes of data were written",
                                     bytes_written, to_write->len);
                            g_assert (error == NULL || *error != NULL);
                            g_string_free (to_write, TRUE);
                            g_object_unref (ostream);
                            return FALSE;
                        }
                        g_string_free (to_write, TRUE);
                        g_free (temp);
                        break;
                    case ET_PLAYLIST_CONTENT_EXTENDED_MASK:
                    {
                        /* Header uses information generated from a mask. */
                        gchar *mask = filename_from_display (gtk_entry_get_text (GTK_ENTRY (priv->content_mask_entry)));
                        /* Special case: do not replace illegal characters and
                         * do not check if there is a directory separator in
                         * the mask. */
                        gchar *filename_generated_utf8 = Scan_Generate_New_Filename_From_Mask (etfile, mask, TRUE);
                        gchar *filename_generated = filename_from_display (filename_generated_utf8);

                        to_write = g_string_new ("#EXTINF:");
                        /* Must be written in system encoding (not UTF-8). */
                        g_string_append_printf (to_write, "%d,%s\r\n", duration,
                                                filename_generated);

                        if (!g_output_stream_write_all (G_OUTPUT_STREAM (ostream),
                                                        to_write->str,
                                                        to_write->len,
                                                        &bytes_written, NULL,
                                                        error))
                        {
                            g_debug ("Only %" G_GSIZE_FORMAT " bytes out of %"
                                     G_GSIZE_FORMAT "bytes of data were written",
                                     bytes_written, to_write->len);
                            g_assert (error == NULL || *error != NULL);
                            g_string_free (to_write, TRUE);
                            g_object_unref (ostream);
                            return FALSE;
                        }
                        g_string_free (to_write, TRUE);
                        g_free (mask);
                        g_free (filename_generated_utf8);
                    }
                }

                /* 3) Write the file path. */
                if (g_settings_get_boolean (MainSettings,
                                            "playlist-dos-separator"))
                {
                    gchar *filename_conv = g_strdup(filename+strlen(basedir)+1);
                    convert_forwardslash_to_backslash (filename_conv);

                    to_write = g_string_new (filename_conv);
                    /* Must be written in system encoding (not UTF-8)*/
                    to_write = g_string_append (to_write, "\r\n");

                    if (!g_output_stream_write_all (G_OUTPUT_STREAM (ostream),
                                                    to_write->str,
                                                    to_write->len,
                                                    &bytes_written, NULL,
                                                    error))
                    {
                        g_debug ("Only %" G_GSIZE_FORMAT " bytes out of %"
                                 G_GSIZE_FORMAT "bytes of data were written",
                                 bytes_written, to_write->len);
                        g_assert (error == NULL || *error != NULL);
                        g_string_free (to_write, TRUE);
                        g_object_unref (ostream);
                        return FALSE;
                    }
                    g_string_free (to_write, TRUE);
                    g_free(filename_conv);
                }else
                {
                    to_write = g_string_new (filename+strlen(basedir)+1);
                    /* Must be written in system encoding (not UTF-8)*/
                    to_write = g_string_append (to_write, "\r\n");

                    if (!g_output_stream_write_all (G_OUTPUT_STREAM (ostream),
                                                    to_write->str,
                                                    to_write->len,
                                                    &bytes_written, NULL,
                                                    error))
                    {
                        g_debug ("Only %" G_GSIZE_FORMAT " bytes out of %"
                                 G_GSIZE_FORMAT "bytes of data were written",
                                 bytes_written, to_write->len);
                        g_assert (error == NULL || *error != NULL);
                        g_string_free (to_write, TRUE);
                        g_object_unref (ostream);
                        return FALSE;
                    }
                    g_string_free (to_write, TRUE);
                }
            }
        }
        else /* !ETSettings:playlist-relative */
        {
            gsize bytes_written;

            /* 2) Write the header. */
            switch (playlist_content)
            {
                case ET_PLAYLIST_CONTENT_FILENAMES:
                    /* No header written. */
                    break;
                case ET_PLAYLIST_CONTENT_EXTENDED:
                    /* Header has extended information. */
                    temp = g_path_get_basename (filename);
                    to_write = g_string_new ("#EXTINF:");
                    /* Must be written in system encoding (not UTF-8). */
                    g_string_append_printf (to_write, "%d,%s\r\n", duration,
                                            temp);
                    g_free (temp);

                    if (!g_output_stream_write_all (G_OUTPUT_STREAM (ostream),
                                                    to_write->str, to_write->len,
                                                    &bytes_written, NULL, error))
                    {
                        g_debug ("Only %" G_GSIZE_FORMAT " bytes out of %"
                                 G_GSIZE_FORMAT" bytes of data were written",
                                 bytes_written, to_write->len);
                        g_assert (error == NULL || *error != NULL);
                        g_string_free (to_write, TRUE);
                        g_object_unref (ostream);
                        return FALSE;
                    }
                    g_string_free (to_write, TRUE);
                    break;
                case ET_PLAYLIST_CONTENT_EXTENDED_MASK:
                {
                    /* Header uses information generated from a mask. */
                    gchar *mask = filename_from_display (gtk_entry_get_text (GTK_ENTRY (priv->content_mask_entry)));
                    /* Special case: do not replace illegal characters and
                     * do not check if there is a directory separator in
                     * the mask. */
                    gchar *filename_generated_utf8 = Scan_Generate_New_Filename_From_Mask (etfile, mask, TRUE);
                    gchar *filename_generated = filename_from_display (filename_generated_utf8);

                    to_write = g_string_new ("#EXTINF:");
                    /* Must be written in system encoding (not UTF-8). */
                    g_string_append_printf (to_write, "%d,%s\r\n", duration,
                                            filename_generated);
                    g_free (filename_generated);

                    if (!g_output_stream_write_all (G_OUTPUT_STREAM (ostream),
                                                    to_write->str, to_write->len,
                                                    &bytes_written, NULL, error))
                    {
                        g_debug ("Only %" G_GSIZE_FORMAT " bytes out of %"
                                 G_GSIZE_FORMAT" bytes of data were written",
                                 bytes_written, to_write->len);
                        g_assert (error == NULL || *error != NULL);
                        g_string_free (to_write, TRUE);
                        g_object_unref (ostream);
                        return FALSE;
                    }
                    g_string_free (to_write, TRUE);
                    g_free (mask);
                    g_free (filename_generated_utf8);
                }
                break;
            }

            /* 3) Write the file path. */
            if (g_settings_get_boolean (MainSettings,
                                        "playlist-dos-separator"))
            {
                gchar *filename_conv = g_strdup(filename);
                convert_forwardslash_to_backslash(filename_conv);

                to_write = g_string_new (filename_conv);
                /* Must be written in system encoding (not UTF-8)*/
                to_write = g_string_append (to_write, "\r\n");

                if (!g_output_stream_write_all (G_OUTPUT_STREAM (ostream),
                                                to_write->str, to_write->len,
                                                &bytes_written, NULL, error))
                {
                    g_debug ("Only %" G_GSIZE_FORMAT " bytes out of %"
                             G_GSIZE_FORMAT" bytes of data were written",
                             bytes_written, to_write->len);
                    g_assert (error == NULL || *error != NULL);
                    g_string_free (to_write, TRUE);
                    g_object_unref (ostream);
                    return FALSE;
                }
                g_string_free (to_write, TRUE);
                g_free(filename_conv);
            }else
            {
                to_write = g_string_new (filename);
                /* Must be written in system encoding (not UTF-8)*/
                to_write = g_string_append (to_write, "\r\n");

                if (!g_output_stream_write_all (G_OUTPUT_STREAM (ostream),
                                                to_write->str, to_write->len,
                                                &bytes_written, NULL, error))
                {
                    g_debug ("Only %" G_GSIZE_FORMAT " bytes out of %"
                             G_GSIZE_FORMAT" bytes of data were written",
                             bytes_written, to_write->len);
                    g_assert (error == NULL || *error != NULL);
                    g_string_free (to_write, TRUE);
                    g_object_unref (ostream);
                    return FALSE;
                }
                g_string_free (to_write, TRUE);
            }
        }
    }

    if (g_settings_get_boolean (MainSettings, "playlist-selected-only"))
    {
        g_list_free (etfilelist);
    }

    g_assert (error == NULL || *error == NULL);
    g_object_unref (ostream);
    g_free(basedir);
    return TRUE;
}

static void
write_button_clicked (EtPlaylistDialog *self)
{
    EtPlaylistDialogPrivate *priv;
    gchar *playlist_name = NULL;
    gchar *playlist_path_utf8;      // Path
    gchar *playlist_basename_utf8;  // Filename
    gchar *playlist_name_utf8;      // Path + filename
    gchar *temp;
    GtkWidget *msgdialog;

    priv = et_playlist_dialog_get_instance_private (self);

    /* Check if playlist name was filled. */
    if (g_settings_get_boolean (MainSettings, "playlist-use-mask")
        && g_utf8_strlen (gtk_entry_get_text (GTK_ENTRY(priv->name_mask_entry)), -1) <=0)
    {
        /* TODO: Can this happen? */
        g_settings_set_boolean (MainSettings, "playlist-use-mask", FALSE);
    }

    // Path of the playlist file (may be truncated later if PLAYLIST_CREATE_IN_PARENT_DIR is TRUE)
    playlist_path_utf8 = filename_to_display (et_application_window_get_current_path (ET_APPLICATION_WINDOW (MainWindow)));

    /* Build the playlist filename. */
    if (g_settings_get_boolean (MainSettings, "playlist-use-mask"))
    {
        gchar *playlist_name;
        EtConvertSpaces convert_mode;

        if (!ETCore->ETFileList)
            return;

        playlist_name = g_settings_get_string (MainSettings,
                                               "playlist-filename-mask");

        /* Generate filename from tag of the current selected file (FIXME). */
        temp = filename_from_display (playlist_name);
        g_free (playlist_name);
        playlist_basename_utf8 = Scan_Generate_New_Filename_From_Mask (ETCore->ETFileDisplayed,
                                                                       temp,
                                                                       FALSE);
        g_free (temp);

        /* Replace Characters (with scanner). */
        convert_mode = g_settings_get_flags (MainSettings,
                                             "rename-convert-spaces");

        switch (convert_mode)
        {
            case ET_CONVERT_SPACES_SPACES:
                Scan_Convert_Underscore_Into_Space (playlist_basename_utf8);
                Scan_Convert_P20_Into_Space (playlist_basename_utf8);
                break;
            case ET_CONVERT_SPACES_UNDERSCORES:
                Scan_Convert_Space_Into_Underscore (playlist_basename_utf8);
                break;
            case ET_CONVERT_SPACES_REMOVE:
                Scan_Remove_Spaces (playlist_basename_utf8);
                break;
            default:
                g_assert_not_reached ();
        }
    }else // PLAYLIST_USE_DIR_NAME
    {

        if ( strcmp(playlist_path_utf8,G_DIR_SEPARATOR_S)==0 )
        {
            playlist_basename_utf8 = g_strdup("playlist");
        }else
        {
            gchar *tmp_string = g_strdup(playlist_path_utf8);
            // Remove last '/'
            if (tmp_string[strlen(tmp_string)-1]==G_DIR_SEPARATOR)
                tmp_string[strlen(tmp_string)-1] = '\0';
            // Get directory name
            temp = g_path_get_basename(tmp_string);
            playlist_basename_utf8 = g_strdup(temp);
            g_free(tmp_string);
            g_free(temp);
        }

    }

    /* Must be placed after "Build the playlist filename", as we can truncate
     * the path! */
    if (g_settings_get_boolean (MainSettings, "playlist-parent-directory"))
    {
        if ( (strcmp(playlist_path_utf8,G_DIR_SEPARATOR_S) != 0) )
        {
            gchar *tmp;
            // Remove last '/'
            if (playlist_path_utf8[strlen(playlist_path_utf8)-1]==G_DIR_SEPARATOR)
                playlist_path_utf8[strlen(playlist_path_utf8)-1] = '\0';
            // Get parent directory
            if ( (tmp=strrchr(playlist_path_utf8,G_DIR_SEPARATOR)) != NULL )
                *(tmp + 1) = '\0';
        }
    }

    // Generate path + filename of playlist
    if (playlist_path_utf8[strlen(playlist_path_utf8)-1]==G_DIR_SEPARATOR)
        playlist_name_utf8 = g_strconcat(playlist_path_utf8,playlist_basename_utf8,".m3u",NULL);
    else
        playlist_name_utf8 = g_strconcat(playlist_path_utf8,G_DIR_SEPARATOR_S,playlist_basename_utf8,".m3u",NULL);

    g_free(playlist_path_utf8);
    g_free(playlist_basename_utf8);

    playlist_name = filename_from_display(playlist_name_utf8);

    {
        GFile *file = g_file_new_for_path (playlist_name);
        GError *error = NULL;

        if (!write_playlist (self, file, &error))
        {
            // Writing fails...
            msgdialog = gtk_message_dialog_new (GTK_WINDOW (self),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_CLOSE,
                                               _("Cannot write playlist file '%s'"),
                                               playlist_name_utf8);
            gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (msgdialog),
                                                      "%s", error->message);
            gtk_window_set_title(GTK_WINDOW(msgdialog),_("Playlist File Error"));

            gtk_dialog_run(GTK_DIALOG(msgdialog));
            gtk_widget_destroy(msgdialog);
            g_error_free (error);
        }else
        {
            gchar *msg;
            msg = g_strdup_printf(_("Written playlist file '%s'"),playlist_name_utf8);
            /*msgbox = msg_box_new(_("Information"),
                                   GTK_WINDOW(WritePlaylistWindow),
                                   NULL,
                                   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
								   msg,
								   GTK_STOCK_DIALOG_INFO,
                                   GTK_STOCK_OK, GTK_RESPONSE_OK,
                                   NULL);
            gtk_dialog_run(GTK_DIALOG(msgbox));
            gtk_widget_destroy(msgbox);*/
            Statusbar_Message(msg,TRUE);
            g_free(msg);
        }
        g_object_unref (file);
    }
    g_free(playlist_name_utf8);
    g_free(playlist_name);
}

/*
 * on_response:
 * @dialog: the dialog which emitted the response signal
 * @response_id: the response ID
 * @user_data: user data set when the signal was connected
 *
 * Signal handler for the write playlist dialog.
 */
static void
on_response (GtkDialog *dialog, gint response_id, gpointer user_data)
{
    switch (response_id)
    {
        case GTK_RESPONSE_OK:
            write_button_clicked (ET_PLAYLIST_DIALOG (dialog));
            break;
        case GTK_RESPONSE_CANCEL:
            gtk_widget_hide (GTK_WIDGET (dialog));
            break;
        case GTK_RESPONSE_DELETE_EVENT:
            break;
        default:
            g_assert_not_reached ();
    }
}

/*
 * entry_check_content_mask:
 * @entry: the entry for which to check the mask
 * @user_data: user data set when the signal was connected
 *
 * Display an icon in the entry if the current text contains an invalid mask.
 */
static void
entry_check_content_mask (GtkEntry *entry, gpointer user_data)
{
    gchar *tmp  = NULL;
    gchar *mask = NULL;

    g_return_if_fail (entry != NULL);

    mask = g_strdup (gtk_entry_get_text (entry));
    if (!mask || strlen(mask)<1)
        goto Bad_Mask;

    while (mask)
    {
        if ( (tmp=strrchr(mask,'%'))==NULL )
        {
            /* There is no more code. */
            /* No code in mask is accepted. */
            goto Good_Mask;
        }
        if (strlen(tmp)>1 && (tmp[1]=='t' || tmp[1]=='a' || tmp[1]=='b' || tmp[1]=='y' ||
                              tmp[1]=='g' || tmp[1]=='n' || tmp[1]=='l' || tmp[1]=='c' || tmp[1]=='i'))
        {
            /* The code is valid. */
            /* No separator is accepted. */
            *(mask+strlen(mask)-strlen(tmp)) = '\0';
        }else
        {
            goto Bad_Mask;
        }
    }

    Bad_Mask:
        g_free(mask);
        gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY,
                                           "emblem-unreadable");
        gtk_entry_set_icon_tooltip_text (entry, GTK_ENTRY_ICON_SECONDARY,
                                         _("Invalid scanner mask"));
        return;

    Good_Mask:
        g_free(mask);
        gtk_entry_set_icon_from_icon_name (entry, GTK_ENTRY_ICON_SECONDARY,
                                           NULL);
        return;
}

static void
create_playlist_dialog (EtPlaylistDialog *self)
{
    EtPlaylistDialogPrivate *priv;
    GtkDialog *dialog;
    GtkWidget *frame;
    GtkWidget *content_area;
    GtkWidget *hbox;
    GtkWidget *vbox;
    GtkWidget *playlist_use_mask_name;
    GtkWidget *playlist_use_dir_name;
    GtkWidget *playlist_only_selected_files;
    GtkWidget *playlist_full_path;
    GtkWidget *playlist_relative_path;
    GtkWidget *playlist_create_in_parent_dir;
    GtkWidget *playlist_use_dos_separator;
    GtkWidget *playlist_content_filenames;
    GtkWidget *playlist_content_extended;
    GtkWidget *playlist_content_mask;

    priv = et_playlist_dialog_get_instance_private (self);
    dialog = GTK_DIALOG (self);

    gtk_window_set_title (GTK_WINDOW (self), _("Generate Playlist"));
    gtk_window_set_transient_for (GTK_WINDOW (self), GTK_WINDOW (MainWindow));
    gtk_window_set_destroy_with_parent (GTK_WINDOW (self), TRUE);
    gtk_dialog_add_buttons (dialog, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                            GTK_STOCK_SAVE, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response (dialog, GTK_RESPONSE_OK);
    g_signal_connect (dialog, "response", G_CALLBACK (on_response), NULL);
    g_signal_connect (dialog, "delete-event",
                      G_CALLBACK (gtk_widget_hide_on_delete), NULL);

    content_area = gtk_dialog_get_content_area (dialog);
    gtk_box_set_spacing (GTK_BOX (content_area), BOX_SPACING);
    gtk_container_set_border_width (GTK_CONTAINER (content_area), BOX_SPACING);


    /* Playlist name */
    frame = gtk_frame_new (_("M3U Playlist Name"));
    gtk_box_pack_start (GTK_BOX (content_area), frame, TRUE, TRUE, 0);
    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, BOX_SPACING);
    gtk_container_add (GTK_CONTAINER(frame), vbox);
    gtk_container_set_border_width(GTK_CONTAINER (vbox), 4);

    playlist_use_mask_name = gtk_radio_button_new_with_label(NULL, _("Use mask:"));
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, BOX_SPACING);
    gtk_box_pack_start(GTK_BOX(vbox),hbox,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(hbox),playlist_use_mask_name,FALSE,FALSE,0);
    priv->name_mask_entry = gtk_entry_new ();
    gtk_box_pack_start (GTK_BOX (hbox), priv->name_mask_entry, FALSE, FALSE,
                        0);
    playlist_use_dir_name = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(playlist_use_mask_name),_("Use directory name"));
    gtk_box_pack_start(GTK_BOX(vbox),playlist_use_dir_name,FALSE,FALSE,0);
    g_settings_bind (MainSettings, "playlist-filename-mask",
                     priv->name_mask_entry, "text", G_SETTINGS_BIND_DEFAULT);
    g_settings_bind (MainSettings, "playlist-use-mask", playlist_use_mask_name,
                     "active", G_SETTINGS_BIND_DEFAULT);

    /* Mask status icon. Signal connection to check if mask is correct in the
     * mask entry. */
    g_signal_connect (priv->name_mask_entry, "changed",
                      G_CALLBACK (entry_check_content_mask), NULL);

    /* Playlist options */
    frame = gtk_frame_new (_("Playlist Options"));
    gtk_box_pack_start (GTK_BOX (content_area), frame, TRUE, TRUE, 0);
    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, BOX_SPACING);
    gtk_container_add (GTK_CONTAINER (frame), vbox);
    gtk_container_set_border_width (GTK_CONTAINER (vbox), BOX_SPACING);

    playlist_only_selected_files = gtk_check_button_new_with_label(_("Include only the selected files"));
    gtk_box_pack_start(GTK_BOX(vbox),playlist_only_selected_files,FALSE,FALSE,0);
    g_settings_bind (MainSettings, "playlist-selected-only",
                     playlist_only_selected_files, "active",
                     G_SETTINGS_BIND_DEFAULT);
    gtk_widget_set_tooltip_text(playlist_only_selected_files,_("If activated, only the selected files will be "
        "written in the playlist file. Else, all the files will be written."));

    playlist_full_path = gtk_radio_button_new_with_label(NULL,_("Use full path for files in playlist"));
    gtk_box_pack_start(GTK_BOX(vbox),playlist_full_path,FALSE,FALSE,0);
    playlist_relative_path = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(playlist_full_path),
        _("Use relative path for files in playlist"));
    gtk_box_pack_start(GTK_BOX(vbox),playlist_relative_path,FALSE,FALSE,0);
    g_settings_bind (MainSettings, "playlist-relative", playlist_relative_path,
                     "active", G_SETTINGS_BIND_DEFAULT);

    // Create playlist in parent directory
    playlist_create_in_parent_dir = gtk_check_button_new_with_label(_("Create playlist in the parent directory"));
    gtk_box_pack_start(GTK_BOX(vbox),playlist_create_in_parent_dir,FALSE,FALSE,0);
    g_settings_bind (MainSettings, "playlist-parent-directory",
                     playlist_create_in_parent_dir, "active",
                     G_SETTINGS_BIND_DEFAULT);
    gtk_widget_set_tooltip_text(playlist_create_in_parent_dir,_("If activated, the playlist will be created "
        "in the parent directory."));

    // DOS Separator
    playlist_use_dos_separator = gtk_check_button_new_with_label(_("Use DOS directory separator"));
#ifndef G_OS_WIN32
    /* This makes no sense under Win32, so we do not display it. */
    gtk_box_pack_start(GTK_BOX(vbox),playlist_use_dos_separator,FALSE,FALSE,0);
#endif /* !G_OS_WIN32 */
    g_settings_bind (MainSettings, "playlist-dos-separator",
                     playlist_use_dos_separator, "active",
                     G_SETTINGS_BIND_DEFAULT);
    gtk_widget_set_tooltip_text(playlist_use_dos_separator,_("This option replaces the UNIX directory "
        "separator '/' into DOS separator '\\'."));

    /* Playlist content */
    frame = gtk_frame_new (_("Playlist Content"));
    gtk_box_pack_start (GTK_BOX (content_area), frame, TRUE, TRUE, 0);
    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, BOX_SPACING);
    gtk_container_add (GTK_CONTAINER (frame), vbox);
    gtk_container_set_border_width (GTK_CONTAINER (vbox), BOX_SPACING);

    playlist_content_filenames = gtk_radio_button_new_with_label (NULL,
                                                                  _("Write only list of files"));
    gtk_widget_set_name (playlist_content_filenames, "filenames");
    gtk_box_pack_start (GTK_BOX (vbox), playlist_content_filenames, FALSE,
                        FALSE,0);

    playlist_content_extended = gtk_radio_button_new_with_label_from_widget (GTK_RADIO_BUTTON (playlist_content_filenames),
                                                                             _("Write info using filename"));
    gtk_widget_set_name (playlist_content_extended, "extended");
    gtk_box_pack_start (GTK_BOX (vbox), playlist_content_extended, FALSE,
                        FALSE, 0);

    playlist_content_mask = gtk_radio_button_new_with_label_from_widget (GTK_RADIO_BUTTON (playlist_content_filenames),
                                                                         _("Write info using:"));
    gtk_widget_set_name (playlist_content_mask, "extended-mask");
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, BOX_SPACING);
    gtk_box_pack_start(GTK_BOX(vbox),hbox,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(hbox),playlist_content_mask,FALSE,FALSE,0);
    // Set a label, a combobox and un editor button in the 3rd radio button
    priv->content_mask_entry = gtk_entry_new ();
    gtk_box_pack_start (GTK_BOX (hbox), priv->content_mask_entry, FALSE, FALSE,
                        0);
    g_settings_bind (MainSettings, "playlist-default-mask",
                     priv->content_mask_entry, "text",
                     G_SETTINGS_BIND_DEFAULT);

    /* Mask status icon. Signal connection to check if mask is correct in the
     * mask entry. */
    g_signal_connect (priv->content_mask_entry, "changed",
                      G_CALLBACK (entry_check_content_mask), NULL);

    g_settings_bind_with_mapping (MainSettings, "playlist-content",
                                  playlist_content_filenames, "active",
                                  G_SETTINGS_BIND_DEFAULT,
                                  et_settings_enum_radio_get,
                                  et_settings_enum_radio_set,
                                  playlist_content_filenames, NULL);
    g_settings_bind_with_mapping (MainSettings, "playlist-content",
                                  playlist_content_extended, "active",
                                  G_SETTINGS_BIND_DEFAULT,
                                  et_settings_enum_radio_get,
                                  et_settings_enum_radio_set,
                                  playlist_content_extended, NULL);
    g_settings_bind_with_mapping (MainSettings, "playlist-content",
                                  playlist_content_mask, "active",
                                  G_SETTINGS_BIND_DEFAULT,
                                  et_settings_enum_radio_get,
                                  et_settings_enum_radio_set,
                                  playlist_content_mask, NULL);

    /* To initialize the mask status icon and visibility. */
    g_signal_emit_by_name (priv->name_mask_entry, "changed");
    g_signal_emit_by_name (priv->content_mask_entry, "changed");
}

static void
et_playlist_dialog_finalize (GObject *object)
{
    G_OBJECT_CLASS (et_playlist_dialog_parent_class)->finalize (object);
}

static void
et_playlist_dialog_init (EtPlaylistDialog *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, ET_TYPE_PLAYLIST_DIALOG,
                                              EtPlaylistDialogPrivate);

    create_playlist_dialog (self);
}

static void
et_playlist_dialog_class_init (EtPlaylistDialogClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = et_playlist_dialog_finalize;

    g_type_class_add_private (klass, sizeof (EtPlaylistDialogPrivate));
}

/*
 * et_playlist_dialog_new:
 *
 * Create a new EtPlaylistDialog instance.
 *
 * Returns: a new #EtPlaylistDialog
 */
EtPlaylistDialog *
et_playlist_dialog_new (void)
{
    return g_object_new (ET_TYPE_PLAYLIST_DIALOG, NULL);
}
