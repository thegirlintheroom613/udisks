/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Tomas Bzatek <tbzatek@redhat.com>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include <glib/gi18n-lib.h>

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <glib.h>
#include <glib-object.h>
#include <gmodule.h>

#include "udisksdaemon.h"
#include "udisksdaemonutil.h"
#include "udisksmodulemanager.h"
#include "udisksconfigmanager.h"
#include "udisksprivate.h"
#include "udiskslogging.h"
#include "udisksmodule.h"

/**
 * SECTION:UDisksModuleManager
 * @title: UDisksModuleManager
 * @short_description: Manages daemon modules
 *
 * ## UDisks modular approach # {#udisks-modular-design}
 *
 * UDisks functionality can be extended by modules. It's not a fully
 * pluggable system as we know it, in this case modules are almost integral
 * parts of the source tree. Meaning that modules are free to use whatever
 * internal objects they need as there is no universal module API (or a
 * translation layer).
 *
 * This fact allows us to stay code-wise simple and transparent. It also means
 * that there's no support for out-of-the-tree modules and care must be taken
 * when changing UDisks internals. As a design decision, for sake of simplicity,
 * once modules are loaded they stay active until the daemon exits (this may be
 * a subject to change in the future).
 *
 * The primary motivation for this was to keep the daemon low on resource
 * footprint for basic usage (typically desktop environments) and only
 * activating the extended functionality when needed (e.g. enterprise storage
 * applications). As the extra information comes in form of additional D-Bus
 * objects and interfaces, no difference should be observed by legacy clients.
 *
 * ## D-Bus interface extensibility # {#udisks-modular-design-dbus}
 *
 * The modular approach is fairly simple, there are basically two primary ways
 * of extending the D-Bus API:
 *  * by attaching custom interfaces to existing objects (limited to block and
 *    drive objects for the moment)
 *  * by exporting objects of its own type directly in the object manager root
 *
 * Besides that there are several other ways of extensibility such as attaching
 * custom interfaces on the master /org/freedesktop/UDisks2/Manager object.
 *
 * ## Modules activation # {#udisks-modular-activation}
 *
 * The UDisks daemon constructs a #UDisksModuleManager singleton acting as
 * a manager. This object tracks module usage and takes care of its activation.
 *
 * By default, module manager is constructed on daemon startup but module
 * loading is delayed until requested. This can be overriden by the
 * --force-load-modules and --disable-modules commandline switches that makes
 * modules loaded right on startup or never loaded respectively.
 *
 * Upon successful activation, a "modules-activated" signal is emitted on the
 * #UDisksModuleManager object. Any daemon objects connected to this signal are
 * responsible for performing "coldplug" on their exported objects to assure
 * modules would pick up the devices they're interested in. See e.g.
 * UDisksModuleObjectNewFunc() to see how device binding works for
 * #UDisksModuleObject.
 *
 * Modules are in fact separate shared objects (.so) that are loaded from the
 * "$(libdir)/udisks2/modules" path (usually "/usr/lib/udisks2/modules"). No
 * extra or service files are needed, the directory is enumerated and all files
 * are attempted to be loaded.
 *
 * Clients are supposed to call the org.freedesktop.UDisks2.Manager.EnableModules()
 * D-Bus method as a "greeter" call. Please note that from asynchronous nature
 * of uevents and the way modules are processing them the extra D-Bus interfaces
 * may not be available right after this method call returns.
 *
 * ## Module API # {#udisks-modular-api}
 *
 * The (strictly internal) module API is simple - only a couple of functions
 * are needed. The following text contains brief description of individual
 * parts of the module API with further links to detailed description within
 * this API reference book.
 *
 * The #UDisksModuleManager first loads all module entry functions, i.e.
 * symbols defined in the public facing header "udisksmoduleiface.h". Only
 * those symbols should be exported from each module. The header file is only
 * meant to be compiled in modules, not the daemon. If any of the symbols is
 * missing in the module library, the whole module is skipped.
 *
 * Once module symbols are resolved, module manager activates each module by
 * calling udisks_module_init() on it. The returned so-called "state" pointer
 * is stored in the #UDisksModuleManager and can be later retrieved by calling
 * the udisks_module_manager_get_module_state_pointer() method. This is typically
 * used further in the module code to retrieve and store module-specific runtime
 * data.
 *
 * Every one of the "udisksmoduleiface.h" header file symbols has its counterpart
 * defined in the "udisksmoduleifacetypes.h" header file in form of function
 * pointers. Those are used internally for symbol resolving purposes. However,
 * they also carry detailed documentation. For illustration purposes, let's
 * call these symbol pairs the "module setup entry functions". See
 * #UDisksModuleIfaceSetupFunc, #UDisksModuleObjectNewSetupFunc and
 * #UDisksModuleNewManagerIfaceSetupFunc for reference. These however are
 * essentially auxiliary symbols only described for demonstrating the big
 * picture; for the useful part of the module API please read on.
 *
 * Every module setup entry function (besides the very simple udisks_module_init())
 * returns an array of setup structures or functions, containing either none
 * (NULL result), one or more elements. The result is then mixed by
 * #UDisksModuleManager from all modules and separate lists are created for
 * each kind of UDisks way of extension. Such lists are then used in the daemon
 * code at appropriate places, sequentially calling elements from the lists to
 * obtain data or objects that are then typically exported on D-Bus.
 */


/**
 * UDisksModuleManager:
 *
 * The #UDisksModuleManager structure contains only private data and
 * should only be accessed using the provided API.
 */
struct _UDisksModuleManager
{
  GObject parent_instance;

  UDisksDaemon *daemon;

  GList *modules;
  GMutex modules_lock;

  gboolean uninstalled;
};

typedef struct _UDisksModuleManagerClass UDisksModuleManagerClass;

struct _UDisksModuleManagerClass
{
  GObjectClass parent_class;

  /* Signals */
  void (*modules_activated) (UDisksModuleManager *manager);
};

/*--------------------------------------------------------------------------------------------------------------*/

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_UNINSTALLED,
};

enum
{
  MODULES_ACTIVATED_SIGNAL,
  LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (UDisksModuleManager, udisks_module_manager, G_TYPE_OBJECT)

static void
udisks_module_manager_finalize (GObject *object)
{
  UDisksModuleManager *manager = UDISKS_MODULE_MANAGER (object);

  udisks_module_manager_unload_modules (manager);

  g_mutex_clear (&manager->modules_lock);

  if (G_OBJECT_CLASS (udisks_module_manager_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (udisks_module_manager_parent_class)->finalize (object);
}


static void
udisks_module_manager_init (UDisksModuleManager *manager)
{
  g_return_if_fail (UDISKS_IS_MODULE_MANAGER (manager));

  g_mutex_init (&manager->modules_lock);
}

static gchar *
get_module_sopath_for_name (UDisksModuleManager *manager,
                            const gchar         *module_name)
{
  gchar *module_dir;
  gchar *module_path;
  gchar *lib_filename;

  g_return_val_if_fail (UDISKS_IS_MODULE_MANAGER (manager), NULL);

  if (! udisks_module_manager_get_uninstalled (manager))
    module_dir = g_build_path (G_DIR_SEPARATOR_S, UDISKS_MODULE_DIR, NULL);
  else
    module_dir = g_build_path (G_DIR_SEPARATOR_S, BUILD_DIR, "modules", NULL);

  lib_filename = g_strdup_printf ("lib" PACKAGE_NAME_UDISKS2 "_%s.so", module_name);
  module_path = g_build_filename (G_DIR_SEPARATOR_S,
                                  module_dir,
                                  lib_filename,
                                  NULL);
  g_free (lib_filename);
  g_free (module_dir);

  return module_path;
}

static GList *
get_modules_list (UDisksModuleManager *manager)
{
  UDisksConfigManager *config_manager;
  GDir *dir;
  GError *error = NULL;
  const GList *modules_i = NULL;
  GList *modules_list = NULL;
  const gchar *dent;
  gchar *module_dir;
  gchar *pth;

  g_return_val_if_fail (UDISKS_IS_MODULE_MANAGER (manager), NULL);

  /* Open a directory with modules. */
  if (! udisks_module_manager_get_uninstalled (manager))
    module_dir = g_build_path (G_DIR_SEPARATOR_S, UDISKS_MODULE_DIR, NULL);
  else
    module_dir = g_build_path (G_DIR_SEPARATOR_S, BUILD_DIR, "modules", NULL);
  dir = g_dir_open (module_dir, 0, &error);
  if (! dir)
    {
      udisks_warning ("Error loading modules: %s", error->message);
      g_clear_error (&error);
      g_free (module_dir);
      return NULL;
    }

  config_manager = udisks_daemon_get_config_manager (manager->daemon);
  if (udisks_config_manager_get_modules_all (config_manager))
    {
      /* Load all the modules from modules directory. */
      while ((dent = g_dir_read_name (dir)))
        {
          if (!g_str_has_suffix (dent, ".so"))
            continue;

          pth = g_build_filename (G_DIR_SEPARATOR_S, module_dir, dent, NULL);
          modules_list = g_list_append (modules_list, pth);
        }
    }
  else
    {
      /* Load only those modules which are specified in config file. */
      for (modules_i = udisks_config_manager_get_modules (config_manager);
           modules_i;
           modules_i = modules_i->next)
        {
          pth = get_module_sopath_for_name (manager, modules_i->data);
          modules_list = g_list_append (modules_list, pth);
        }
    }

  g_dir_close (dir);
  g_free (module_dir);

  return modules_list;
}

static gboolean
have_module (UDisksModuleManager *manager,
             const gchar         *module_name)
{
  GList *l;

  for (l = manager->modules; l != NULL; l = g_list_next (l))
    {
      UDisksModule *module = l->data;

      if (g_strcmp0 (udisks_module_get_name (module), module_name) == 0)
        return TRUE;
    }

  return FALSE;
}

static gboolean
load_single_module_unlocked (UDisksModuleManager *manager,
                             const gchar         *sopath,
                             gboolean            *do_notify,
                             GError             **error)
{
  GModule *handle;
  gchar *module_id;
  gchar *module_new_func_name;
  UDisksModuleIDFunc module_id_func;
  UDisksModuleNewFunc module_new_func;
  UDisksModule *module;

  handle = g_module_open (sopath, 0);
  if (handle == NULL)
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "%s",
                   g_module_error ());
      return FALSE;
    }

  if (! g_module_symbol (handle, "udisks_module_id", (gpointer *) &module_id_func))
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "%s: %s", sopath, g_module_error ());
      g_module_close (handle);
      return FALSE;
    }

  module_id = module_id_func ();
  if (have_module (manager, module_id))
    {
      /* module with the same name already loaded, skip */
      udisks_debug ("Module '%s' already loaded, skipping", module_id);
      g_free (module_id);
      g_module_close (handle);
      return TRUE;
    }

  udisks_notice ("Loading module %s ...", module_id);

  module_new_func_name = g_strdup_printf ("udisks_module_%s_new", module_id);
  if (! g_module_symbol (handle, module_new_func_name, (gpointer *) &module_new_func))
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "%s", g_module_error ());
      g_module_close (handle);
      g_free (module_new_func_name);
      g_free (module_id);
      return FALSE;
    }
  g_free (module_new_func_name);

  /* The following calls will initialize new GType's from the module,
   * making it uneligible for unload.
   */
  g_module_make_resident (handle);

  module = module_new_func (manager->daemon,
                            NULL /* cancellable */,
                            error);
  if (module == NULL)
    {
      /* Workaround for broken modules to avoid segfault */
      if (error == NULL)
        {
          g_set_error_literal (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                               "unknown fatal error");
        }
      g_free (module_id);
      g_module_close (handle);
      return FALSE;
    }

  manager->modules = g_list_append (manager->modules, module);
  g_free (module_id);

  *do_notify = TRUE;
  return TRUE;
}

/**
 * udisks_module_manager_load_single_module:
 * @manager: A #UDisksModuleManager instance.
 * @name: Module name.
 * @error: Return location for error or %NULL.
 *
 * Loads single module and emits the "modules-activated" signal in case
 * the module activation was successful. Already active module is not being
 * reinitialized on subsequent calls to this method and %TRUE is returned
 * immediately.
 *
 * Returns: %TRUE if module was activated successfully, %FALSE otherwise with @error being set.
 */
gboolean
udisks_module_manager_load_single_module (UDisksModuleManager *manager,
                                          const gchar         *name,
                                          GError             **error)
{
  gchar *module_path;
  gboolean do_notify = FALSE;
  gboolean ret;

  g_return_val_if_fail (UDISKS_IS_MODULE_MANAGER (manager), FALSE);

  module_path = get_module_sopath_for_name (manager, name);
  if (module_path == NULL)
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Cannot determine module path for '%s'",
                   name);
      return FALSE;
    }

  g_mutex_lock (&manager->modules_lock);
  ret = load_single_module_unlocked (manager, module_path, &do_notify, error);
  g_mutex_unlock (&manager->modules_lock);

  g_free (module_path);

  if (do_notify)
    {
      /* This will run connected signal handlers synchronously, i.e.
       * performs coldplug on all existing objects within #UDisksLinuxProvider.
       */
      g_signal_emit (manager, signals[MODULES_ACTIVATED_SIGNAL], 0);
    }

  return ret;
}

/**
 * udisks_module_manager_load_modules:
 * @manager: A #UDisksModuleManager instance.
 *
 * Loads all modules at a time and emits the "modules-activated" signal in case
 * any new module has been activated. Modules that are already loaded are skipped
 * on subsequent calls to this method.
 */
void
udisks_module_manager_load_modules (UDisksModuleManager *manager)
{
  GList *modules_to_load;
  GList *modules_to_load_tmp;
  GError *error = NULL;
  gboolean do_notify = FALSE;

  g_return_if_fail (UDISKS_IS_MODULE_MANAGER (manager));

  g_mutex_lock (&manager->modules_lock);

  /* Load the modules */
  modules_to_load = get_modules_list (manager);
  for (modules_to_load_tmp = modules_to_load;
       modules_to_load_tmp;
       modules_to_load_tmp = modules_to_load_tmp->next)
    {

      if (! load_single_module_unlocked (manager,
                                         modules_to_load_tmp->data,
                                         &do_notify,
                                         &error))
        {
          udisks_critical ("Error loading module: %s",
                           error->message);
          g_clear_error (&error);
          continue;
        }
    }

  g_mutex_unlock (&manager->modules_lock);

  g_list_free_full (modules_to_load, (GDestroyNotify) g_free);

  /* Emit 'modules-activated' in case new modules have been loaded. */
  if (do_notify)
    g_signal_emit (manager, signals[MODULES_ACTIVATED_SIGNAL], 0);
}

/**
 * udisks_module_manager_unload_modules:
 * @manager: A #UDisksModuleManager instance.
 *
 * Unloads all modules at a time. A "modules-activated" signal is emitted if
 * there are any modules active to give listeners room to unexport all module
 * interfaces and objects. The udisks_module_manager_get_modules() would
 * return NULL at that time. Note that proper module unload is not fully
 * supported, this is just a convenience call for cleanup.
 */
void
udisks_module_manager_unload_modules (UDisksModuleManager *manager)
{
  GList *l;

  g_return_if_fail (UDISKS_IS_MODULE_MANAGER (manager));

  g_mutex_lock (&manager->modules_lock);

  l = g_steal_pointer (&manager->modules);
  if (l)
    {
      /* notify listeners that the list of active modules has changed */
      g_signal_emit (manager, signals[MODULES_ACTIVATED_SIGNAL], 0);
    }
  /* only unref module objects after all listeners have performed cleanup */
  g_list_free_full (l, g_object_unref);

  g_mutex_unlock (&manager->modules_lock);
}

static void
udisks_module_manager_constructed (GObject *object)
{
  if (! g_module_supported ())
    {
      udisks_warning ("Modules are unsupported on the current platform");
      return;
    }

  if (G_OBJECT_CLASS (udisks_module_manager_parent_class)->constructed != NULL)
    (*G_OBJECT_CLASS (udisks_module_manager_parent_class)->constructed) (object);
}

static void
udisks_module_manager_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  UDisksModuleManager *manager = UDISKS_MODULE_MANAGER (object);

  switch (prop_id)
    {
    case PROP_DAEMON:
      g_value_set_object (value, udisks_module_manager_get_daemon (manager));
      break;

    case PROP_UNINSTALLED:
      g_value_set_boolean (value, manager->uninstalled);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
udisks_module_manager_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  UDisksModuleManager *manager = UDISKS_MODULE_MANAGER (object);

  switch (prop_id)
    {
    case PROP_DAEMON:
      g_assert (manager->daemon == NULL);
      /* We don't take a reference to the daemon */
      manager->daemon = g_value_get_object (value);
      break;

    case PROP_UNINSTALLED:
      manager->uninstalled = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
udisks_module_manager_class_init (UDisksModuleManagerClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->finalize     = udisks_module_manager_finalize;
  gobject_class->constructed  = udisks_module_manager_constructed;
  gobject_class->get_property = udisks_module_manager_get_property;
  gobject_class->set_property = udisks_module_manager_set_property;

  /**
   * UDisksModuleManager:daemon:
   *
   * The #UDisksDaemon for the object.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The daemon for the object",
                                                        UDISKS_TYPE_DAEMON,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksModuleManager:uninstalled:
   *
   * Loads modules from the build directory.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_UNINSTALLED,
                                   g_param_spec_boolean ("uninstalled",
                                                         "Load modules from the build directory",
                                                         "Whether the modules should be loaded from the build directory",
                                                         FALSE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY));

  /**
   * UDisksModuleManager:modules-activated:
   * @manager: A #UDisksModuleManager.
   *
   * Emitted after new modules have been activated.
   *
   * This signal is emitted in the <link linkend="g-main-context-push-thread-default">thread-default main loop</link> of the thread that @manager was created in.
   */
  signals[MODULES_ACTIVATED_SIGNAL] = g_signal_new ("modules-activated",
                                                    G_TYPE_FROM_CLASS (klass),
                                                    G_SIGNAL_RUN_LAST,
                                                    G_STRUCT_OFFSET (UDisksModuleManagerClass, modules_activated),
                                                    NULL,
                                                    NULL,
                                                    g_cclosure_marshal_generic,
                                                    G_TYPE_NONE,
                                                    0);
}

/**
 * udisks_module_manager_new:
 * @daemon: A #UDisksDaemon instance.
 *
 * Creates a new #UDisksModuleManager object.
 *
 * Returns: A #UDisksModuleManager. Free with g_object_unref().
 */
UDisksModuleManager *
udisks_module_manager_new (UDisksDaemon *daemon)
{
  return UDISKS_MODULE_MANAGER (g_object_new (UDISKS_TYPE_MODULE_MANAGER,
                                              "daemon", daemon, NULL));
}

/**
 * udisks_module_manager_new_uninstalled:
 * @daemon: A #UDisksDaemon instance.
 *
 * Creates a new #UDisksModuleManager object.
 *
 * Returns: A #UDisksModuleManager. Free with g_object_notify().
 */
UDisksModuleManager *
udisks_module_manager_new_uninstalled (UDisksDaemon *daemon)
{
  return UDISKS_MODULE_MANAGER (g_object_new (UDISKS_TYPE_MODULE_MANAGER,
                                              "daemon", daemon,
                                              "uninstalled", TRUE, NULL));
}

/**
 * udisks_module_manager_get_daemon:
 * @manager: A #UDisksModuleManager.
 *
 * Gets the daemon used by @manager.
 *
 * Returns: A #UDisksDaemon. Do not free, the object is owned by @manager.
 */
UDisksDaemon *
udisks_module_manager_get_daemon (UDisksModuleManager *manager)
{
  g_return_val_if_fail (UDISKS_IS_MODULE_MANAGER (manager), NULL);
  return manager->daemon;
}

gboolean
udisks_module_manager_get_uninstalled (UDisksModuleManager *manager)
{
  g_return_val_if_fail (UDISKS_IS_MODULE_MANAGER (manager), FALSE);
  return manager->uninstalled;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_module_manager_get_modules:
 * @manager: A #UDisksModuleManager instance.
 *
 * Gets list of active modules. Can be called from different threads.
 *
 * Returns: (transfer full) (nullable) (element-type UDisksModule) : A list of #UDisksModule
 *          or %NULL if no modules are presently loaded.  Free the elements
 *          with g_object_unref().
 */
GList *
udisks_module_manager_get_modules (UDisksModuleManager *manager)
{
  GList *l;

  g_return_val_if_fail (UDISKS_IS_MODULE_MANAGER (manager), NULL);

  /* Return fast to avoid bottleneck over locking, expecting
   * a simple pointer check whould be atomic.
   */
  if (manager->modules == NULL)
    return NULL;

  g_mutex_lock (&manager->modules_lock);
  l = g_list_copy_deep (manager->modules, (GCopyFunc) udisks_g_object_ref_copy, NULL);
  g_mutex_unlock (&manager->modules_lock);

  return l;
}
