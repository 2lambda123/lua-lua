/*
** $Id: lua.c,v 1.133 2004/11/18 19:53:49 roberto Exp $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*/


#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define lua_c

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"


/*
** generic extra include file
*/
#ifdef LUA_USERCONFIG
#include LUA_USERCONFIG
#endif




static lua_State *globalL = NULL;

static const char *progname = PROGNAME;



static void lstop (lua_State *L, lua_Debug *ar) {
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);
  luaL_error(L, "interrupted!");
}


static void laction (int i) {
  signal(i, SIG_DFL); /* if another SIGINT happens before lstop,
                              terminate process (default action) */
  lua_sethook(globalL, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}


static void print_usage (void) {
  fprintf(stderr,
  "usage: %s [options] [script [args]].\n"
  "Available options are:\n"
  "  -        execute stdin as a file\n"
  "  -e stat  execute string `stat'\n"
  "  -i       enter interactive mode after executing `script'\n"
  "  -l name  load and run library `name'\n"
  "  -v       show version information\n"
  "  --       stop handling options\n" ,
  progname);
}


static void l_message (const char *pname, const char *msg) {
  if (pname) fprintf(stderr, "%s: ", pname);
  fprintf(stderr, "%s\n", msg);
}


static int report (lua_State *L, int status) {
  if (status && !lua_isnil(L, -1)) {
    const char *msg = lua_tostring(L, -1);
    if (msg == NULL) msg = "(error object is not a string)";
    l_message(progname, msg);
    lua_pop(L, 1);
  }
  return status;
}


static int docall (lua_State *L, int narg, int clear) {
  int status;
  int base = lua_gettop(L) - narg;  /* function index */
  lua_pushliteral(L, "_TRACEBACK");
  lua_rawget(L, LUA_GLOBALSINDEX);  /* get traceback function */
  lua_insert(L, base);  /* put it under chunk and args */
  signal(SIGINT, laction);
  status = lua_pcall(L, narg, (clear ? 0 : LUA_MULTRET), base);
  signal(SIGINT, SIG_DFL);
  lua_remove(L, base);  /* remove traceback function */
  return status;
}


static void print_version (void) {
  l_message(NULL, LUA_VERSION "  " LUA_COPYRIGHT);
}


static int getargs (lua_State *L, char *argv[], int n) {
  int i, narg;
  for (i=n+1; argv[i]; i++) {
    luaL_checkstack(L, 1, "too many arguments to script");
    lua_pushstring(L, argv[i]);
  }
  narg = i-(n+1);  /* number of arguments to the script (not to `lua.c') */
  lua_newtable(L);
  for (i=0; argv[i]; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - n);
  }
  return narg;
}


static int dofile (lua_State *L, const char *name) {
  int status = luaL_loadfile(L, name) || docall(L, 0, 1);
  return report(L, status);
}


static int dostring (lua_State *L, const char *s, const char *name) {
  int status = luaL_loadbuffer(L, s, strlen(s), name) || docall(L, 0, 1);
  return report(L, status);
}


static int dolibrary (lua_State *L, const char *name) {
  luaL_getfield(L, LUA_GLOBALSINDEX, "package.path");
  if (!lua_isstring(L, -1)) {
    l_message(progname, "`package.path' must be a string");
    return 1;
  }
  name = luaL_searchpath(L, name, lua_tostring(L, -1));
  if (name == NULL) return report(L, 1);
  else  return dofile(L, name);
}



/*
** this macro defines a function to show the prompt and reads the
** next line for manual input
*/
#ifndef lua_readline
#define lua_readline(L,prompt)		readline(L,prompt)

/* maximum length of an input line */
#ifndef MAXINPUT
#define MAXINPUT	512
#endif


static int readline (lua_State *L, const char *prompt) {
  static char buffer[MAXINPUT];
  if (prompt) {
    fputs(prompt, stdout);
    fflush(stdout);
  }
  if (fgets(buffer, sizeof(buffer), stdin) == NULL)
    return 0;  /* read fails */
  else {
    lua_pushstring(L, buffer);
    return 1;
  }
}

#endif


static const char *get_prompt (lua_State *L, int firstline) {
  const char *p = NULL;
  lua_pushstring(L, firstline ? "_PROMPT" : "_PROMPT2");
  lua_rawget(L, LUA_GLOBALSINDEX);
  p = lua_tostring(L, -1);
  if (p == NULL) p = (firstline ? PROMPT : PROMPT2);
  lua_pop(L, 1);  /* remove global */
  return p;
}


static int incomplete (lua_State *L, int status) {
  if (status == LUA_ERRSYNTAX &&
         strstr(lua_tostring(L, -1), "near `<eof>'") != NULL) {
    lua_pop(L, 1);
    return 1;
  }
  else
    return 0;
}


static int loadline (lua_State *L) {
  int status;
  lua_settop(L, 0);
  if (lua_readline(L, get_prompt(L, 1)) == 0)  /* no input? */
    return -1;
  if (lua_tostring(L, -1)[0] == '=') {  /* line starts with `=' ? */
    lua_pushfstring(L, "return %s", lua_tostring(L, -1)+1);/* `=' -> `return' */
    lua_remove(L, -2);  /* remove original line */
  }
  for (;;) {  /* repeat until gets a complete line */
    status = luaL_loadbuffer(L, lua_tostring(L, 1), lua_strlen(L, 1), "=stdin");
    if (!incomplete(L, status)) break;  /* cannot try to add lines? */
    if (lua_readline(L, get_prompt(L, 0)) == 0)  /* no more input? */
      return -1;
    lua_concat(L, lua_gettop(L));  /* join lines */
  }
  lua_saveline(L, lua_tostring(L, 1));
  lua_remove(L, 1);  /* remove line */
  return status;
}


static void dotty (lua_State *L) {
  int status;
  const char *oldprogname = progname;
  progname = NULL;
  print_version();
  while ((status = loadline(L)) != -1) {
    if (status == 0) status = docall(L, 0, 0);
    report(L, status);
    if (status == 0 && lua_gettop(L) > 0) {  /* any result to print? */
      lua_getglobal(L, "print");
      lua_insert(L, 1);
      if (lua_pcall(L, lua_gettop(L)-1, 0, 0) != 0)
        l_message(progname, lua_pushfstring(L, "error calling `print' (%s)",
                                               lua_tostring(L, -1)));
    }
  }
  lua_settop(L, 0);  /* clear stack */
  fputs("\n", stdout);
  progname = oldprogname;
}


static int checkvar (lua_State *L) {
  const char *name = lua_tostring(L, 2);
  if (name)
    luaL_error(L, "attempt to access undefined variable `%s'", name);
  return 0;
}


#define clearinteractive(i)	(*i &= 2)

static int handle_argv (lua_State *L, char *argv[], int *interactive) {
  if (argv[1] == NULL) {  /* no arguments? */
    *interactive = 0;
    if (stdin_is_tty())
      dotty(L);
    else
      dofile(L, NULL);  /* executes stdin as a file */
  }
  else {  /* other arguments; loop over them */
    int i;
    for (i = 1; argv[i] != NULL; i++) {
      if (argv[i][0] != '-') break;  /* not an option? */
      switch (argv[i][1]) {  /* option */
        case '-': {  /* `--' */
          if (argv[i][2] != '\0') {
            print_usage();
            return 1;
          }
          i++;  /* skip this argument */
          goto endloop;  /* stop handling arguments */
        }
        case '\0': {
          clearinteractive(interactive);
          dofile(L, NULL);  /* executes stdin as a file */
          break;
        }
        case 'i': {
          *interactive = 2;  /* force interactive mode after arguments */
          break;
        }
        case 'v': {
          clearinteractive(interactive);
          print_version();
          break;
        }
        case 'w': {
          if (lua_getmetatable(L, LUA_GLOBALSINDEX)) {
            lua_pushcfunction(L, checkvar);
            lua_setfield(L, -2, "__index");
          }
          break;
        }
        case 'e': {
          const char *chunk = argv[i] + 2;
          clearinteractive(interactive);
          if (*chunk == '\0') chunk = argv[++i];
          if (chunk == NULL) {
            print_usage();
            return 1;
          }
          if (dostring(L, chunk, "=(command line)") != 0)
            return 1;
          break;
        }
        case 'l': {
          const char *filename = argv[i] + 2;
          if (*filename == '\0') filename = argv[++i];
          if (filename == NULL) {
            print_usage();
            return 1;
          }
          if (dolibrary(L, filename))
            return 1;  /* stop if file fails */
          break;
        }
        default: {
          clearinteractive(interactive);
          print_usage();
          return 1;
        }
      }
    } endloop:
    if (argv[i] != NULL) {
      const char *filename = argv[i];
      int narg = getargs(L, argv, i);  /* collect arguments */
      int status;
      lua_setglobal(L, "arg");
      clearinteractive(interactive);
      status = luaL_loadfile(L, filename);
      lua_insert(L, -(narg+1));
      if (status == 0)
        status = docall(L, narg, 0);
      else
        lua_pop(L, narg);      
      return report(L, status);
    }
  }
  return 0;
}


static int handle_luainit (lua_State *L) {
  const char *init = getenv("LUA_INIT");
  if (init == NULL) return 0;  /* status OK */
  else if (init[0] == '@')
    return dofile(L, init+1);
  else
    return dostring(L, init, "=LUA_INIT");
}


struct Smain {
  int argc;
  char **argv;
  int status;
};


static int pmain (lua_State *L) {
  struct Smain *s = (struct Smain *)lua_touserdata(L, 1);
  int status;
  int interactive = 1;
  if (s->argv[0] && s->argv[0][0]) progname = s->argv[0];
  globalL = L;
  lua_userinit(L);  /* open libraries */
  status = handle_luainit(L);
  if (status == 0) {
    status = handle_argv(L, s->argv, &interactive);
    if (status == 0 && interactive) dotty(L);
  }
  s->status = status;
  return 0;
}


int main (int argc, char *argv[]) {
  int status;
  struct Smain s;
  lua_State *L = lua_open();  /* create state */
  if (L == NULL) {
    l_message(argv[0], "cannot create state: not enough memory");
    return EXIT_FAILURE;
  }
  s.argc = argc;
  s.argv = argv;
  status = lua_cpcall(L, &pmain, &s);
  report(L, status);
  lua_close(L);
  return (status || s.status) ? EXIT_FAILURE : EXIT_SUCCESS;
}

