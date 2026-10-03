// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gtk-menu-probe.m — standalone in-process protocol probe for the
 * "defer exact native menu actions through a one-shot GLib idle" experiment in
 * gtk application-quartz-menu.c (baseline: deepseek-work/pdf-menu-20260920/
 * baseline/gtkapplication-quartz-menu.c, didSelectItem: lines 302-317).
 *
 * WHAT THIS IS
 *   An outcome test that drives the *actual* Cocoa NSMenu item path
 *   (-[NSMenu performActionForItemAtIndex:]) built by GTK's quartz backend from
 *   a real GtkApplication menubar, and observes action counters. It never calls
 *   a GAction directly.
 *
 * WHAT THIS IS NOT
 *   It does not click the real menu bar and does not traverse MenuBarAgent, the
 *   native NSMenu tracking run loop or WindowServer. A pass here is a unit /
 *   protocol pass only; the root's real native-click GUI requirement stands.
 *
 * CONTRACT UNDER TEST (candidate)
 *   Only the exact action names win.document-open, win.document-import and
 *   app.file-open-window are deferred through one one-shot GLib idle on the
 *   default main context. Every other native action stays synchronous. The
 *   deferred dispatch captures the original focus widget/root weakly plus the
 *   borrowed action name and an owned target GVariant, fires exactly once per
 *   click (no per-item coalescing), survives removal of the source menu item,
 *   drops safely if the original window is destroyed, and never retargets to a
 *   subsequently activated window.
 *
 * EXIT CODES
 *   0  all six cases PASS (candidate behavior present)
 *   2  setup failure (menu/item/focus/action not constructed) — investigate
 *      the harness, not the candidate
 *   3  zero setup errors but >=1 case FAIL — the probe reports a failure, not
 *      a pass (a synchronous, non-deferring behavior fails these cases)
 *   1  internal / usage error
 *
 * Build and run: see run-gtk-menu-probe.py in this directory.
 */

#import <AppKit/AppKit.h>

#include <gtk/gtk.h>

#include <stdio.h>
#include <string.h>

enum
{
  PROBE_EXIT_ALL_PASS = 0,
  PROBE_EXIT_INTERNAL = 1,
  PROBE_EXIT_SETUP    = 2,
  PROBE_EXIT_EXPECTED = 3
};

typedef struct
{
  const char *name;
  GtkWidget  *window;          /* owned by the application */
  int         document_open;
  int         document_import;
  int         probe_sync;
  int         param_ok;
  int         param_bad;
  char        last_param[128];
} WinState;

static int g_setup_errors = 0;
static int g_cases_total = 0;
static int g_cases_passed = 0;
static int g_cases_failed = 0;

static int g_harness_idle_count = 0;

static int g_app_file_open_window_count = 0;
static int g_app_param_bad = 0;
static char g_app_last_param[128];

static void
setup_fail (const char *what)
{
  g_setup_errors++;
  fprintf (stderr, "PROBE_SETUP result=FAIL what=%s\n", what);
  fflush (stderr);
}

/* ------------------------------------------------------------------ actions */

static void
on_document_open (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  WinState *ws = user_data;
  (void) action;

  ws->document_open++;

  if (parameter != NULL && g_variant_is_of_type (parameter, G_VARIANT_TYPE_STRING))
    {
      const char *s = g_variant_get_string (parameter, NULL);
      g_strlcpy (ws->last_param, s, sizeof ws->last_param);
      if (g_strcmp0 (s, "PARAM-REMOVE") == 0)
        ws->param_ok++;
      fprintf (stderr,
               "PROBE_TRACE fired win.document-open on %s count=%d param=%s\n",
               ws->name, ws->document_open, s);
    }
  else
    {
      ws->param_bad++;
      fprintf (stderr,
               "PROBE_TRACE fired win.document-open on %s count=%d param=<none>\n",
               ws->name, ws->document_open);
    }
  fflush (stderr);
}

static void
on_document_import (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
  WinState *ws = user_data;
  (void) action;
  (void) parameter;
  ws->document_import++;
  fprintf (stderr, "PROBE_TRACE fired win.document-import on %s count=%d\n",
           ws->name, ws->document_import);
  fflush (stderr);
}

static void
on_probe_sync (GSimpleAction *action,
               GVariant      *parameter,
               gpointer       user_data)
{
  WinState *ws = user_data;
  (void) action;
  (void) parameter;
  ws->probe_sync++;
  fprintf (stderr, "PROBE_TRACE fired win.probe-sync on %s count=%d\n",
           ws->name, ws->probe_sync);
  fflush (stderr);
}

static void
on_app_file_open_window (GSimpleAction *action,
                         GVariant      *parameter,
                         gpointer       user_data)
{
  (void) action;
  (void) user_data;

  g_app_file_open_window_count++;

  if (parameter != NULL && g_variant_is_of_type (parameter, G_VARIANT_TYPE_STRING))
    {
      const char *s = g_variant_get_string (parameter, NULL);
      g_strlcpy (g_app_last_param, s, sizeof g_app_last_param);
      fprintf (stderr, "PROBE_TRACE fired app.file-open-window count=%d param=%s\n",
               g_app_file_open_window_count, s);
    }
  else
    {
      g_app_param_bad++;
      fprintf (stderr, "PROBE_TRACE fired app.file-open-window count=%d param=<none>\n",
               g_app_file_open_window_count);
    }
  fflush (stderr);
}

/* -------------------------------------------------------------------- menu  */

static void
append_action_item (GMenu      *menu,
                    const char *label,
                    const char *action,
                    GVariant   *target /* floating, nullable */)
{
  GMenuItem *item = g_menu_item_new (label, NULL);

  g_menu_item_set_action_and_target_value (item, action, target);
  g_menu_append_item (menu, item);
  g_object_unref (item);
}

static int
find_menu_model_index_by_label (GMenuModel *model,
                                const char *label)
{
  int n = g_menu_model_get_n_items (model);

  for (int i = 0; i < n; i++)
    {
      GVariant *v = g_menu_model_get_item_attribute_value (model, i,
                                                           G_MENU_ATTRIBUTE_LABEL,
                                                           G_VARIANT_TYPE_STRING);
      if (v == NULL)
        continue;

      gboolean match = (g_strcmp0 (g_variant_get_string (v, NULL), label) == 0);
      g_variant_unref (v);

      if (match)
        return i;
    }

  return -1;
}

/* ------------------------------------------------------------- NSMenu lookup */

/* Locate an observed NSMenuItem by title recursively; returns the owning menu
 * and the item index inside it (no guessed indexes). */
static BOOL
find_menu_item_recursive (NSMenu    *menu,
                          NSString  *title,
                          NSMenu   **out_parent,
                          NSInteger *out_index)
{
  NSArray<NSMenuItem *> *items = [menu itemArray];
  NSUInteger n = [items count];

  for (NSUInteger i = 0; i < n; i++)
    {
      NSMenuItem *item = [items objectAtIndex:i];

      if ([[item title] isEqualToString:title])
        {
          if (out_parent != NULL)
            *out_parent = menu;
          if (out_index != NULL)
            *out_index = (NSInteger) i;
          return YES;
        }

      NSMenu *sub = [item submenu];
      if (sub != nil &&
          find_menu_item_recursive (sub, title, out_parent, out_index))
        return YES;
    }

  return NO;
}

static NSMenu *
find_submenu_for_title (NSMenu    *root,
                        NSString  *title)
{
  NSMenu *parent = nil;
  NSInteger index = -1;

  if (!find_menu_item_recursive (root, title, &parent, &index))
    return nil;

  return [[[parent itemArray] objectAtIndex:index] submenu];
}

/* Re-locate the observed item by title immediately before each activation:
 * menu mutations between cases shift indexes, so cached indexes are unsafe. */
static BOOL
activate_item_by_title (NSMenu    *menu,
                        NSString  *title)
{
  NSMenu *parent = nil;
  NSInteger index = -1;

  if (!find_menu_item_recursive (menu, title, &parent, &index))
    return NO;

  [parent performActionForItemAtIndex:index];
  return YES;
}

/* --------------------------------------------------------------- main loop  */

/* Bounded, non-blocking: never enters the Cocoa run loop and never blocks on a
 * GLib poll. 128 iterations is far more than one idle needs. */
static void
pump_default_context (void)
{
  for (int i = 0; i < 128; i++)
    {
      (void) g_main_context_iteration (NULL, FALSE);
      g_usleep (1000);
    }
}

static void
record_case (int         number,
             const char *name,
             gboolean    ok,
             const char *detail)
{
  g_cases_total++;
  if (ok)
    g_cases_passed++;
  else
    g_cases_failed++;

  fprintf (stdout, "PROBE_CASE case=%d name=%s result=%s detail=%s\n",
           number, name, ok ? "PASS" : "FAIL", detail);
  fflush (stdout);
}

static gboolean
harness_idle_cb (gpointer data)
{
  (void) data;
  g_harness_idle_count++;
  return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------- windows   */

static const GActionEntry win_entries[] = {
  { "document-open",   on_document_open,   "s",  NULL, NULL },
  { "document-import", on_document_import, NULL, NULL, NULL },
  { "probe-sync",      on_probe_sync,      NULL, NULL, NULL },
};

static GtkWidget *
create_probe_window (GtkApplication *app,
                     WinState       *ws,
                     GtkWidget     **out_button)
{
  GtkWidget *win = gtk_application_window_new (app);
  GtkWidget *button = gtk_button_new_with_label ("focus");
  GSimpleActionGroup *dummy = g_simple_action_group_new ();

  ws->window = win;

  g_action_map_add_action_entries (G_ACTION_MAP (win), win_entries,
                                   G_N_ELEMENTS (win_entries), ws);

  gtk_window_set_child (GTK_WINDOW (win), button);

  /* Force creation of the focus widget's action muxer so win./app. prefixes
   * resolve; without this a plain GtkButton has no muxer of its own and
   * gtk_widget_activate_action_variant() would silently no-op. */
  gtk_widget_insert_action_group (button, "probe-focus", G_ACTION_GROUP (dummy));
  g_object_unref (dummy);

  /* Real application windows are realized. Realize (without mapping) so any
   * "original window is still live/unrealized" dispatch guard is exercised the
   * same way it is in the app; a never-realized probe window could otherwise
   * cause a false drop. */
  gtk_widget_realize (win);

  if (!gtk_widget_get_realized (win))
    setup_fail ("realized window did not become realized");

  if (!gtk_widget_grab_focus (button))
    setup_fail ("gtk_widget_grab_focus returned FALSE");

  if (gtk_window_get_focus (GTK_WINDOW (win)) != button)
    setup_fail ("gtk_window_get_focus did not return the probe button");

  if (out_button != NULL)
    *out_button = button;

  return win;
}

/* -------------------------------------------------------------------- main  */

/* gtk_application_set_menubar() requires a registered non-remote application;
 * GtkApplication docs say the startup signal is the place to call it. GTK's
 * quartz startup (which builds the NSMenu from this model) runs after the
 * user startup signal, so this is then read. */
static void
on_app_startup (GApplication *application,
                gpointer      user_data)
{
  GMenu *menubar = user_data;

  gtk_application_set_menubar (GTK_APPLICATION (application),
                               G_MENU_MODEL (menubar));
}

int
main (int   argc,
      char *argv[])
{
  @autoreleasepool
    {
      const GActionEntry app_entries[] = {
        { "file-open-window", on_app_file_open_window, "s", NULL, NULL },
      };

      GtkApplication *app;
      GMenu *menubar;
      GMenu *file;
      GError *error = NULL;
      NSMenu *main_menu;
      NSMenu *file_menu;
      NSMenu *p_sync = nil, *p_defer = nil, *p_param = nil, *p_app = nil;
      NSInteger i_sync = -1, i_defer = -1, i_param = -1, i_app = -1;
      WinState wa;
      GtkWidget *w_a = NULL;
      GtkWidget *b_a = NULL;

      (void) argc;
      (void) argv;

      memset (&wa, 0, sizeof wa);
      wa.name = "A";

      app = gtk_application_new ("com.vacards.pdfmenuprobe",
                                 G_APPLICATION_NON_UNIQUE);
      if (app == NULL)
        {
          fprintf (stderr, "PROBE_SETUP result=FAIL what=gtk_application_new\n");
          return PROBE_EXIT_INTERNAL;
        }

      g_action_map_add_action_entries (G_ACTION_MAP (app), app_entries,
                                       G_N_ELEMENTS (app_entries), NULL);

      menubar = g_menu_new ();
      file = g_menu_new ();
      append_action_item (file, "PROBE SYNC", "win.probe-sync", NULL);
      append_action_item (file, "PROBE DEFER", "win.document-open",
                          g_variant_new_string ("DEFER-1"));
      append_action_item (file, "PROBE PARAM REMOVE", "win.document-open",
                          g_variant_new_string ("PARAM-REMOVE"));
      append_action_item (file, "PROBE APP FILE", "app.file-open-window",
                          g_variant_new_string ("APP-1"));
      append_action_item (file, "PROBE IMPORT", "win.document-import", NULL);
      g_menu_append_submenu (menubar, "File", G_MENU_MODEL (file));

      g_signal_connect (app, "startup", G_CALLBACK (on_app_startup), menubar);

      /* g_application_register() runs GtkApplication startup: the user startup
       * signal sets the menubar, then the quartz backend builds the real NSMenu
       * from it. No g_application_run, no main loop. */
      if (!g_application_register (G_APPLICATION (app), NULL, &error))
        {
          fprintf (stderr, "PROBE_SETUP result=FAIL what=g_application_register %s\n",
                   error != NULL ? error->message : "<no error>");
          g_clear_error (&error);
          return PROBE_EXIT_SETUP;
        }

      main_menu = [NSApp mainMenu];
      if (main_menu == nil)
        setup_fail ("NSApp mainMenu nil after startup");
      else
        {
          file_menu = find_submenu_for_title (main_menu, @"File");
          if (file_menu == nil)
            setup_fail ("File submenu not found in NSApp mainMenu");
          else
            {
              if (!find_menu_item_recursive (file_menu, @"PROBE SYNC",
                                             &p_sync, &i_sync))
                setup_fail ("NSMenuItem PROBE SYNC not found");
              if (!find_menu_item_recursive (file_menu, @"PROBE DEFER",
                                             &p_defer, &i_defer))
                setup_fail ("NSMenuItem PROBE DEFER not found");
              if (!find_menu_item_recursive (file_menu, @"PROBE PARAM REMOVE",
                                             &p_param, &i_param))
                setup_fail ("NSMenuItem PROBE PARAM REMOVE not found");
              if (!find_menu_item_recursive (file_menu, @"PROBE APP FILE",
                                             &p_app, &i_app))
                setup_fail ("NSMenuItem PROBE APP FILE not found");
              if (!find_menu_item_recursive (file_menu, @"PROBE IMPORT",
                                             NULL, NULL))
                setup_fail ("NSMenuItem PROBE IMPORT not found");
            }
        }

      /* Window A is created before the cases so a focus failure aborts the run. */
      w_a = create_probe_window (app, &wa, &b_a);

      if (gtk_application_get_active_window (app) != GTK_WINDOW (w_a))
        setup_fail ("window A is not the active window");

      /* Harness self-check: the bounded non-blocking pump must actually dispatch
       * a default-idle source on the default main context, otherwise a correct
       * deferred candidate would look like a failure. */
      {
        guint id = g_idle_add (harness_idle_cb, NULL);
        if (id == 0)
          setup_fail ("g_idle_add failed");
        pump_default_context ();
        if (g_harness_idle_count != 1)
          setup_fail ("default-idle source did not dispatch under pump_default_context");
      }

      if (g_setup_errors > 0)
        {
          fprintf (stdout,
                   "PROBE_SUMMARY total=0 passed=0 failed=0 setup_errors=%d\n",
                   g_setup_errors);
          fflush (stdout);
          return PROBE_EXIT_SETUP;
        }

      /* Case 1 — non-allowlisted action must stay synchronous. */
      {
        int before = wa.probe_sync;
        gboolean ok;
        char detail[128];

        if (!activate_item_by_title (file_menu, @"PROBE SYNC"))
          setup_fail ("could not activate PROBE SYNC");
        ok = (wa.probe_sync == before + 1);
        snprintf (detail, sizeof detail, "after_click=%d expected=1",
                  wa.probe_sync - before);
        record_case (1, "nonallowlisted_synchronous", ok, detail);

        pump_default_context ();
        if (wa.probe_sync != before + 1)
          setup_fail ("non-allowlisted action ran a second time after iteration");
      }

      /* Case 2 — allowlisted deferred, one run per click, no coalescing. */
      {
        int before = wa.document_open;
        int after_first;
        int after_three;
        int after_iteration;
        gboolean ok;
        char detail[192];

        if (!activate_item_by_title (file_menu, @"PROBE DEFER"))
          setup_fail ("could not activate PROBE DEFER (1)");
        after_first = wa.document_open - before;

        if (!activate_item_by_title (file_menu, @"PROBE DEFER"))
          setup_fail ("could not activate PROBE DEFER (2)");
        if (!activate_item_by_title (file_menu, @"PROBE DEFER"))
          setup_fail ("could not activate PROBE DEFER (3)");
        after_three = wa.document_open - before;

        pump_default_context ();
        after_iteration = wa.document_open - before;

        ok = (after_first == 0) && (after_three == 0) && (after_iteration == 3);
        snprintf (detail, sizeof detail,
                  "after_first_click=%d after_three_clicks=%d after_iteration=%d "
                  "expected_after_iteration=3",
                  after_first, after_three, after_iteration);
        record_case (2, "allowlisted_deferred_no_coalesce", ok, detail);
      }

      /* Case 3 — parameter must survive removal of the source menu item. */
      {
        int before = wa.document_open;
        int after_click;
        int after_iteration;
        int model_index;
        gboolean ok;
        char detail[192];

        wa.param_ok = 0;
        wa.param_bad = 0;

        if (!activate_item_by_title (file_menu, @"PROBE PARAM REMOVE"))
          setup_fail ("could not activate PROBE PARAM REMOVE");
        after_click = wa.document_open - before;

        /* Free the tracker item and its owned action target before the idle. */
        model_index = find_menu_model_index_by_label (G_MENU_MODEL (file),
                                                      "PROBE PARAM REMOVE");
        if (model_index < 0)
          setup_fail ("GMenu model item PROBE PARAM REMOVE not found");
        else
          g_menu_remove (file, model_index);

        pump_default_context ();
        after_iteration = wa.document_open - before;

        ok = (after_click == 0) && (after_iteration == 1) &&
             (wa.param_ok == 1) && (wa.param_bad == 0);
        snprintf (detail, sizeof detail,
                  "after_click=%d after_iteration=%d param_ok=%d param_bad=%d "
                  "last_param=%s",
                  after_click, after_iteration, wa.param_ok, wa.param_bad,
                  wa.last_param[0] != '\0' ? wa.last_param : "<none>");
        record_case (3, "parameter_survives_removal", ok, detail);
      }

      /* Case 4 — the other two allowlisted actions (app + import) are deferred. */
      {
        int app_before = g_app_file_open_window_count;
        int import_before = wa.document_import;
        int app_after_click;
        int import_after_click;
        int app_after_iteration;
        int import_after_iteration;
        gboolean ok;
        char detail[256];

        if (!activate_item_by_title (file_menu, @"PROBE APP FILE"))
          setup_fail ("could not activate PROBE APP FILE");
        if (!activate_item_by_title (file_menu, @"PROBE IMPORT"))
          setup_fail ("could not activate PROBE IMPORT");

        app_after_click = g_app_file_open_window_count - app_before;
        import_after_click = wa.document_import - import_before;

        pump_default_context ();
        app_after_iteration = g_app_file_open_window_count - app_before;
        import_after_iteration = wa.document_import - import_before;

        ok = (app_after_click == 0) && (import_after_click == 0) &&
             (app_after_iteration == 1) && (import_after_iteration == 1) &&
             (g_app_param_bad == 0) &&
             (g_strcmp0 (g_app_last_param, "APP-1") == 0);
        snprintf (detail, sizeof detail,
                  "app_after_click=%d app_after_iteration=%d "
                  "import_after_click=%d import_after_iteration=%d "
                  "param_bad=%d last_param=%s",
                  app_after_click, app_after_iteration,
                  import_after_click, import_after_iteration,
                  g_app_param_bad,
                  g_app_last_param[0] != '\0' ? g_app_last_param : "<none>");
        record_case (4, "app_and_import_actions_deferred", ok, detail);
      }

      /* Case 5 — destroying the source window before the idle must drop safely. */
      {
        WinState wc;
        GtkWidget *w_c;
        GtkWidget *b_c;
        int before;
        int after_click;
        int after_iteration;
        gboolean ok;
        char detail[192];

        memset (&wc, 0, sizeof wc);
        wc.name = "C";
        w_c = create_probe_window (app, &wc, &b_c);

        if (gtk_application_get_active_window (app) != GTK_WINDOW (w_c))
          setup_fail ("window C is not the active window");

        before = wc.document_open;
        if (!activate_item_by_title (file_menu, @"PROBE DEFER"))
          setup_fail ("could not activate PROBE DEFER (case 5)");
        after_click = wc.document_open - before;

        gtk_window_destroy (GTK_WINDOW (w_c));

        pump_default_context ();
        after_iteration = wc.document_open - before;

        ok = (after_click == 0) && (after_iteration == 0);
        snprintf (detail, sizeof detail,
                  "after_click=%d after_iteration=%d expected_after_iteration=0",
                  after_click, after_iteration);
        record_case (5, "closed_window_drops", ok, detail);
      }

      /* Case 6 — a newer active window must not become the activation target. */
      {
        WinState we;
        WinState wf;
        GtkWidget *w_e;
        GtkWidget *w_f;
        GtkWidget *b_e;
        GtkWidget *b_f;
        int before_e;
        int before_f;
        int after_click_e;
        int after_iteration_e;
        int after_iteration_f;
        gboolean ok;
        char detail[224];

        memset (&we, 0, sizeof we);
        we.name = "E";
        w_e = create_probe_window (app, &we, &b_e);

        before_e = we.document_open;
        if (!activate_item_by_title (file_menu, @"PROBE DEFER"))
          setup_fail ("could not activate PROBE DEFER (case 6)");
        after_click_e = we.document_open - before_e;

        /* Newest added window is gtk_application_get_active_window(). */
        memset (&wf, 0, sizeof wf);
        wf.name = "F";
        w_f = create_probe_window (app, &wf, &b_f);
        before_f = wf.document_open;

        if (gtk_application_get_active_window (app) != GTK_WINDOW (w_f))
          setup_fail ("window F is not the active window");

        pump_default_context ();
        after_iteration_e = we.document_open - before_e;
        after_iteration_f = wf.document_open - before_f;

        ok = (after_click_e == 0) && (after_iteration_e == 1) &&
             (after_iteration_f == 0);
        snprintf (detail, sizeof detail,
                  "E_after_click=%d E_after_iteration=%d F_after_iteration=%d",
                  after_click_e, after_iteration_e, after_iteration_f);
        record_case (6, "no_retarget_to_new_active_window", ok, detail);
      }

      fprintf (stdout,
               "PROBE_SUMMARY total=%d passed=%d failed=%d setup_errors=%d\n",
               g_cases_total, g_cases_passed, g_cases_failed, g_setup_errors);
      fflush (stdout);

      if (g_setup_errors > 0)
        return PROBE_EXIT_SETUP;
      if (g_cases_failed > 0)
        return PROBE_EXIT_EXPECTED;
      return PROBE_EXIT_ALL_PASS;
    }
}
