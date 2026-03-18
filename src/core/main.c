#include <stdatomic.h>
#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/wasmfs.h>
#include <jslib.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAIL_IF_STATUS_EXCEPTION(status)                                       \
  if (PyStatus_Exception(status)) {                                            \
    goto finally;                                                              \
  }

// Initialize python. exit() and print message to stderr on failure.
static void
initialize_python(int argc, char** argv)
{
  bool success = false;
  PyStatus status;

  PyPreConfig preconfig;
  PyPreConfig_InitPythonConfig(&preconfig);

  status = Py_PreInitializeFromBytesArgs(&preconfig, argc, argv);
  FAIL_IF_STATUS_EXCEPTION(status);

  PyConfig config;
  PyConfig_InitPythonConfig(&config);

  status = PyConfig_SetBytesArgv(&config, argc, argv);
  FAIL_IF_STATUS_EXCEPTION(status);

  status = PyConfig_SetBytesString(&config, &config.home, "/");
  FAIL_IF_STATUS_EXCEPTION(status);

  config.write_bytecode = false;
  status = Py_InitializeFromConfig(&config);
  FAIL_IF_STATUS_EXCEPTION(status);

  success = true;
finally:
  PyConfig_Clear(&config);
  if (!success) {
    // This will exit().
    Py_ExitStatusException(status);
  }
}

PyObject*
PyInit__pyodide_core(void);

/**
 * Set up WasmFS.
 */
EMSCRIPTEN_KEEPALIVE void
setup_wasmfs(void)
{
  // WasmFS already creates a memory-backed root with /dev during its init.
}

/**
 * Create a directory and all missing parent directories (like `mkdir -p`).
 * Ignores EEXIST at each level. Returns 0 on success, -errno on failure.
 */
static int
mkdirp(const char* path)
{
  char tmp[PATH_MAX];
  size_t len = strlen(path);
  if (len == 0 || len >= PATH_MAX) {
    return -ENAMETOOLONG;
  }
  memcpy(tmp, path, len + 1);
  for (char* p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        return -errno;
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
    return -errno;
  }
  return 0;
}

static bool opfs_initialized = false;

/**
 * Lazily create the OPFS backend and mount it at /opfs (once).
 * Subsequent calls are no-ops. Requires JSPI.
 */
static int
ensure_opfs_mounted(void)
{
  if (opfs_initialized) {
    return 0;
  }
  backend_t backend = wasmfs_create_opfs_backend();
  if (!backend) {
    return -ENOMEM;
  }
  int ret = wasmfs_create_directory("/opfs", 0777, backend);
  if (ret != 0) {
    return ret;
  }
  opfs_initialized = true;
  return 0;
}

/**
 * Mount an OPFS subdirectory at a VFS path via a symlink.
 *
 * On the first call, creates a single OPFS backend mounted at /opfs.
 * Then creates /opfs<opfs_path> (with all parent dirs) and symlinks
 * mount_path -> /opfs<opfs_path>.
 *
 * Example: pyodide_mount_opfs(s, "/data1", "/data1")
 *   - mounts OPFS at /opfs (once)
 *   - mkdir -p /opfs/data1
 *   - symlink /data1 -> /opfs/data1
 *
 * Requires JSPI. Returns 0 on success, -errno on failure.
 */
EMSCRIPTEN_KEEPALIVE int
pyodide_mount_opfs(JsVal suspender, const char* mount_path, const char* opfs_path)
{
  int ret = ensure_opfs_mounted();
  if (ret != 0) {
    return ret;
  }

  // Build the full OPFS path: /opfs<opfs_path>
  char full_opfs_path[PATH_MAX];
  int n = snprintf(full_opfs_path, sizeof(full_opfs_path), "/opfs%s", opfs_path);
  if (n < 0 || n >= (int)sizeof(full_opfs_path)) {
    return -ENAMETOOLONG;
  }

  // Create the OPFS subdirectory (and all parents under /opfs)
  ret = mkdirp(full_opfs_path);
  if (ret != 0) {
    return ret;
  }

  // Create parent directories of mount_path (everything before the last /)
  char parent[PATH_MAX];
  memcpy(parent, mount_path, strlen(mount_path) + 1);
  char* last_slash = strrchr(parent, '/');
  if (last_slash && last_slash != parent) {
    *last_slash = '\0';
    ret = mkdirp(parent);
    if (ret != 0) {
      return ret;
    }
  }

  // Create symlink: mount_path -> /opfs<opfs_path>
  if (symlink(full_opfs_path, mount_path) != 0) {
    return -errno;
  }
  return 0;
}

/**
 * Mount a Node.js filesystem backend at emscripten_path backed by host_path.
 * Returns 0 on success or a negative errno on failure.
 */
EMSCRIPTEN_KEEPALIVE int
pyodide_mount_node_fs(const char* emscripten_path, const char* host_path)
{
  backend_t backend = wasmfs_create_node_backend(host_path);
  if (!backend) {
    return -1;
  }
  return wasmfs_create_directory(emscripten_path, 0777, backend);
}

/**
 * Unmount a WasmFS mount point. Returns 0 on success or a negative errno.
 */
EMSCRIPTEN_KEEPALIVE int
pyodide_unmount(const char* path)
{
  return wasmfs_unmount(path);
}

/**
 * Bootstrap steps here:
 *  1. Import _pyodide package (we depend on this in _pyodide_core)
 *  2. Initialize the different ffi components and create the _pyodide_core
 *     module
 *  3. Create a PyProxy wrapper around _pyodide package so that JavaScript can
 *     call into _pyodide._base.eval_code and
 *     _pyodide._import_hook.register_js_finder (this happens in loadPyodide in
 *     pyodide.js)
 */
int
main(int argc, char** argv)
{
  setup_wasmfs();
  // This exits and prints a message to stderr on failure,
  // no status code to check.
  PyImport_AppendInittab("_pyodide_core", PyInit__pyodide_core);
  initialize_python(argc, argv);
  // Normally the runtime would exit when main() returns, don't let that
  // happen.
  emscripten_runtime_keepalive_push();
  return 0;
}

void
pymain_run_python(int* exitcode);

EMSCRIPTEN_KEEPALIVE int
run_main()
{
  int exitcode;
  // run_python may call exit() if `-h` or `-V` have been passed. If we stop it
  // from exiting, we'll segfault. So pop the keep alive, so that exit() will
  // call onExit and shut down the runtime. We notice this in pyodide.ts and
  // throw a ExitStatus error.
  emscripten_runtime_keepalive_pop();
  pymain_run_python(&exitcode);
  emscripten_runtime_keepalive_push();
  return exitcode;
}

void
set_suspender(JsVal suspender);

/**
 * call _pyproxy_apply but save the error flag into the argument so it can't be
 * observed by unrelated Python callframes. callPyObjectKwargsSuspending will
 * restore the error flag before calling pythonexc2js(). See
 * test_stack_switching.test_throw_from_switcher for a detailed explanation.
 */
EMSCRIPTEN_KEEPALIVE int
run_main_promising(JsVal suspender)
{
  set_suspender(suspender);
  return run_main();
}
