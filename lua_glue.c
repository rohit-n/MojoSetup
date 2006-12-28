#include "universal.h"
#include "lua_glue.h"
#include "platform.h"
#include "fileio.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "gui.h"

#define MOJOSETUP_NAMESPACE "MojoSetup"

static lua_State *luaState = NULL;

// Allocator interface for internal Lua use.
static void *MojoLua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    if (nsize == 0)
    {
        free(ptr);
        return NULL;
    } // if
    return xrealloc(ptr, nsize);
} // MojoLua_alloc


// Read data from a MojoInput when loading Lua code.
static const char *MojoLua_reader(lua_State *L, void *data, size_t *size)
{
    MojoInput *in = (MojoInput *) data;
    char *retval = (char *) scratchbuf_128k;
    int64 br = in->read(in, scratchbuf_128k, sizeof (scratchbuf_128k));
    if (br <= 0)  // eof or error? (lua doesn't care which?!)
    {
        br = 0;
        retval = NULL;
    } // if

    *size = (size_t) br;
    return retval;
} // MojoLua_reader


static inline int snprintfcat(char **ptr, size_t *len, const char *fmt, ...)
{
    int bw = 0;
    va_list ap;
    va_start(ap, fmt);
    bw = vsnprintf(*ptr, *len, fmt, ap);
    va_end(ap);
    *ptr += bw;
    *len -= bw;
    return bw;
} // snprintfcat


static int luahook_stackwalk(lua_State *L)
{
    const char *errstr = lua_tostring(L, 1);
    lua_Debug ldbg;
    int i = 0;

    if (errstr != NULL)
        logDebug("%s\n", errstr);

    logDebug("Lua stack backtrace:");

    // start at 1 to skip this function.
    for (i = 1; lua_getstack(L, i, &ldbg); i++)
    {
        char *ptr = (char *) scratchbuf_128k;
        size_t len = sizeof (scratchbuf_128k);
        int bw = snprintfcat(&ptr, &len, "#%d", i-1);
        const int maxspacing = 4;
        int spacing = maxspacing - bw;
        while (spacing-- > 0)
            snprintfcat(&ptr, &len, " ");

        if (!lua_getinfo(L, "nSl", &ldbg))
        {
            snprintfcat(&ptr, &len, "???\n");
            logDebug((const char *) scratchbuf_128k);
            continue;
        } // if

        if (ldbg.namewhat[0])
            snprintfcat(&ptr, &len, "%s ", ldbg.namewhat);

        if ((ldbg.name) && (ldbg.name[0]))
            snprintfcat(&ptr, &len, "function %s ()", ldbg.name);
        else
        {
            if (strcmp(ldbg.what, "main") == 0)
                snprintfcat(&ptr, &len, "mainline of chunk");
            else if (strcmp(ldbg.what, "tail") == 0)
                snprintfcat(&ptr, &len, "tail call");
            else
                snprintfcat(&ptr, &len, "unidentifiable function");
        } // if

        logDebug((const char *) scratchbuf_128k);
        ptr = (char *) scratchbuf_128k;
        len = sizeof (scratchbuf_128k);

        for (spacing = 0; spacing < maxspacing; spacing++)
            snprintfcat(&ptr, &len, " ");

        if (strcmp(ldbg.what, "C") == 0)
            snprintfcat(&ptr, &len, "in native code");
        else if (strcmp(ldbg.what, "tail") == 0)
            snprintfcat(&ptr, &len, "in Lua code");
        else if ( (strcmp(ldbg.source, "=?") == 0) && (ldbg.currentline == 0) )
            snprintfcat(&ptr, &len, "in Lua code (debug info stripped)");
        else
        {
            snprintfcat(&ptr, &len, "in Lua code at %s", ldbg.short_src);
            if (ldbg.currentline != -1)
                snprintfcat(&ptr, &len, ":%d", ldbg.currentline);
        } // else
        logDebug((const char *) scratchbuf_128k);
    } // for

    lua_pushstring(L, errstr ? errstr : "");
    return 1;
} // luahook_stackwalk


// This just lets you punch in one-liners and Lua will run them as individual
//  chunks, but you can completely access all Lua state, including calling C
//  functions and altering tables. At this time, it's more of a "console"
//  than a debugger. You can do "p MojoLua_debugger()" from gdb to launch this
//  from a breakpoint in native code, or call MojoSetup.debugger() to launch
//  it from Lua code (with stacktrace intact, too: type 'bt' to see it).
static int luahook_debugger(lua_State *L)
{
#if DISABLE_LUA_PARSER
    logError("Lua debugger is disabled in this build (no parser).");
#else
    int origtop;

    lua_pushcfunction(luaState, luahook_stackwalk);
    origtop = lua_gettop(L);

    printf("Quick and dirty Lua debugger. Type 'exit' to quit.\n");

    while (true)
    {
        char *buf = (char *) scratchbuf_128k;
        int len = 0;
        printf("> ");
        fflush(stdout);
        if (fgets(buf, sizeof (scratchbuf_128k), stdin) == NULL)
        {
            printf("\n\n  fgets() on stdin failed: ");
            break;
        } // if

        len = (int) (strlen(buf) - 1);
        while ( (len >= 0) && ((buf[len] == '\n') || (buf[len] == '\r')) )
            buf[len--] = '\0';

        if (strcmp(buf, "q") == 0)
            break;
        else if (strcmp(buf, "exit") == 0)
            break;
        else if (strcmp(buf, "bt") == 0)
            strcpy(buf, "MojoSetup.stackwalk()");

        if ( (luaL_loadstring(L, buf) != 0) ||
             (lua_pcall(luaState, 0, LUA_MULTRET, -2) != 0) )
        {
            printf("%s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } // if
        else
        {
            printf("Returned %d values.\n", lua_gettop(L) - origtop);
            while (lua_gettop(L) != origtop)
            {
                // !!! FIXME: dump details of values to stdout here.
                lua_pop(L, 1);
            } // while
            printf("\n");
        } // else
    } // while

    lua_pop(L, 1);
    printf("exiting debugger...\n");
#endif

    return 0;
} // luahook_debugger


void MojoLua_debugger(void)
{
    luahook_debugger(luaState);
} // MojoLua_debugger


boolean MojoLua_runFile(const char *basefname)
{
    MojoArchive *ar = GBaseArchive;   // in case we want to generalize later.
    const MojoArchiveEntryInfo *entinfo;
    boolean retval = false;
    char clua[128];  // compiled filename.
    char ulua[128];  // uncompiled filename.
    int rc = 0;
    MojoInput *io = NULL;

    if (snprintf(clua, sizeof (clua), "%s.luac", basefname) >= sizeof (clua))
        return false;

    if (snprintf(ulua, sizeof (ulua), "%s.lua", basefname) >= sizeof (ulua))
        return false;

    if (ar->enumerate(ar, "lua"))
    {
        while ((entinfo = ar->enumNext(ar)) != NULL)
        {
            boolean match = (strcmp(entinfo->filename, clua) == 0);
            #if !DISABLE_LUA_PARSER
            if (!match)
                match = (strcmp(entinfo->filename, ulua) == 0);
            #endif

            if (match)
            {
                if (entinfo->type == MOJOARCHIVE_ENTRY_FILE)
                    io = ar->openCurrentEntry(ar);
                break;
            } // if
        } // while
    } // if

    if (io != NULL)
    {
        char *realfname = xmalloc(strlen(entinfo->filename) + 6);
        sprintf(realfname, "@lua/%s", entinfo->filename);
        lua_pushcfunction(luaState, luahook_stackwalk);
        rc = lua_load(luaState, MojoLua_reader, io, realfname);
        free(realfname);
        io->close(io);

        if (rc != 0)
            lua_error(luaState);
        else
        {
            // Call new chunk on top of the stack (lua_pcall will pop it off).
            if (lua_pcall(luaState, 0, 0, -2) != 0)  // retvals are dumped.
                lua_error(luaState);   // error on stack has debug info.
            else
                retval = true;   // if this didn't panic, we succeeded.
        } // if
        lua_pop(luaState, 1);   // dump stackwalker.
    } // if

    return retval;
} // MojoLua_runFile


void MojoLua_collectGarbage(void)
{
    lua_State *L = luaState;
    uint32 ticks = 0;
    int pre = 0;
    int post = 0;

    pre = (lua_gc(L, LUA_GCCOUNT, 0) * 1024) + lua_gc(L, LUA_GCCOUNTB, 0);
    logDebug("Collecting garbage (currently using %d bytes).", pre);
    ticks = MojoPlatform_ticks();
    lua_gc (L, LUA_GCCOLLECT, 0);
    profile("Garbage collection", ticks);
    post = (lua_gc(L, LUA_GCCOUNT, 0) * 1024) + lua_gc(L, LUA_GCCOUNTB, 0);
    logDebug("Now using %d bytes (%d bytes savings).\n", post, pre - post);
} // MojoLua_collectGarbage


// You can trigger the garbage collector with more control in the standard
//  Lua runtime, but this notes profiling and statistics via logDebug().
static int luahook_collectgarbage(lua_State *L)
{
    MojoLua_collectGarbage();
    return 0;
} // luahook_collectgarbage


static inline void pushStringOrNil(lua_State *L, const char *str)
{
    if (str != NULL)
        lua_pushstring(L, str);
    else
        lua_pushnil(L);
} // pushStringOrNil


// Since localization is kept in Lua tables, I stuck this in the Lua glue.
const char *translate(const char *str)
{
    const char *retval = str;

    if (luaState != NULL)  // No translations before Lua is initialized.
    {
        if (lua_checkstack(luaState, 3))
        {
            int popcount = 0;
            lua_getglobal(luaState, MOJOSETUP_NAMESPACE); popcount++;
            if (lua_istable(luaState, -1))  // namespace is sane?
            {
                lua_getfield(luaState, -1, "translations"); popcount++;
                if (lua_istable(luaState, -1))  // translation table is sane?
                {
                    const char *tr = NULL;
                    lua_getfield(luaState, -1, str); popcount++;
                    tr = lua_tostring(luaState, -1);
                    if (tr != NULL)  // translated for this locale?
                    {
                        char *dst = (char *) scratchbuf_128k;
                        xstrncpy(dst, tr, sizeof(scratchbuf_128k));
                        retval = dst;
                    } // if
                } // if
            } // if
            lua_pop(luaState, popcount);   // remove our stack salsa.
        } // if
    } // if

    return retval;
} // translate


// Use this instead of Lua's error() function if you don't have a
//  programatic error, so you don't get stack callback stuff:
// MojoSetup.fatal("You need the base game to install this expansion pack.")
//  Doesn't actually return.
static int luahook_fatal(lua_State *L)
{
    const char *errstr = lua_tostring(L, 1);
    if (errstr == NULL)
        errstr = _("Unknown error");
    return fatal(errstr);  // doesn't actually return.
    const char *err = luaL_checkstring(L, 1);
    fatal(err);
    return 0;
} // luahook_fatal


// Lua interface to MojoLua_runFile(). This is needed instead of Lua's
//  require(), since it can access scripts inside an archive.
static int luahook_runfile(lua_State *L)
{
    const char *fname = luaL_checkstring(L, 1);
    lua_pushboolean(L, MojoLua_runFile(fname));
    return 1;
} // luahook_runfile


// Lua interface to translate().
static int luahook_translate(lua_State *L)
{
    const char *str = luaL_checkstring(L, 1);
    lua_pushstring(L, translate(str));
    return 1;
} // luahook_translate


static int luahook_ticks(lua_State *L)
{
    lua_pushnumber(L, MojoPlatform_ticks());
    return 1;
} // luahook_ticks


static int luahook_msgbox(lua_State *L)
{
    if (GGui != NULL)
    {
        const char *title = luaL_checkstring(L, 1);
        const char *text = luaL_checkstring(L, 2);
        GGui->msgbox(title, text);
    } // if
    return 0;
} // luahook_msgbox


static int luahook_promptyn(lua_State *L)
{
    boolean rc = false;
    if (GGui != NULL)
    {
        const char *title = luaL_checkstring(L, 1);
        const char *text = luaL_checkstring(L, 2);
        rc = GGui->promptyn(title, text);
    } // if

    lua_pushboolean(L, rc);
    return 1;
} // luahook_msgbox


static int luahook_logwarning(lua_State *L)
{
    logWarning(luaL_checkstring(L, 1));
    return 0;
} // luahook_logwarning


static int luahook_logerror(lua_State *L)
{
    logError(luaL_checkstring(L, 1));
    return 0;
} // luahook_logerror


static int luahook_loginfo(lua_State *L)
{
    logInfo(luaL_checkstring(L, 1));
    return 0;
} // luahook_loginfo


static int luahook_logdebug(lua_State *L)
{
    logDebug(luaL_checkstring(L, 1));
    return 0;
} // luahook_logdebug


static int luahook_cmdline(lua_State *L)
{
    const char *arg = luaL_checkstring(L, 1);
    lua_pushboolean(L, cmdline(arg));
    return 1;
} // luahook_cmdline


static int luahook_cmdlinestr(lua_State *L)
{
    const int argc = lua_gettop(L);
    const char *arg = luaL_checkstring(L, 1);
    const char *envr = (argc < 2) ? NULL : lua_tostring(L, 2);
    const char *deflt = (argc < 3) ? NULL : lua_tostring(L, 3);
    pushStringOrNil(L, cmdlinestr(arg, envr, deflt));
    return 1;
} // luahook_cmdlinestr


static int luahook_gui_start(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    const char *splash = lua_tostring(L, 2);
    lua_pushboolean(L, GGui->start(title, splash));
    return 1;
} // luahook_gui_start


static const uint8 *loadFile(const char *fname, size_t *len)
{
    uint8 *retval = NULL;
    MojoInput *io = MojoInput_newFromArchivePath(GBaseArchive, fname);
    if (io != NULL)
    {
        int64 len64 = io->length(io);
        *len = (size_t) len64;
        if (*len == len64)
        {
            retval = (uint8 *) xmalloc(*len + 1);
            if (io->read(io, retval, *len) == *len)
                retval[*len] = '\0';
            else
            {
                free(retval);
                retval = NULL;
            } // else
        } // if
        io->close(io);
    } // if

    return retval;
} // loadFile

static inline boolean canGoBack(int thisstage)
{
    return (thisstage > 1);
} // canGoBack

static inline boolean canGoForward(int thisstage, int maxstage)
{
    return (thisstage < maxstage);
} // canGoForward


static int luahook_gui_readme(lua_State *L)
{
    size_t len = 0;
    const char *name = luaL_checkstring(L, 1);
    const char *fname = luaL_checkstring(L, 2);
    const int thisstage = luaL_checkinteger(L, 3);
    const int maxstage = luaL_checkinteger(L, 4);
    const uint8 *data = loadFile(fname, &len);
    const boolean can_go_back = canGoBack(thisstage);
    const boolean can_go_fwd = canGoForward(thisstage, maxstage);

    if (data == NULL)
        fatal(_("failed to load file '%s'"), fname);

    lua_pushboolean(L, GGui->readme(name, data, len, can_go_back, can_go_fwd));
    free((void *) data);
    return 1;
} // luahook_gui_readme


static int luahook_gui_stop(lua_State *L)
{
    GGui->stop();
    return 0;
} // luahook_gui_stop


typedef MojoGuiSetupOptions GuiOptions;   // a little less chatty.


static inline uint64 file_size_from_string(const char *str)
{
    // !!! FIXME: this is WRONG
    uint64 retval = (uint64) atoi(str);
    size_t len = strlen(str);
    if (len > 0)
    {
        const uint64 ui64_1024 = (uint64) 1024;
        char ch = str[len-1];

        if ((ch >= 'a') && (ch <= 'z'))
            ch -= ('a' - 'A');

        if (ch == 'K')
            retval *= ui64_1024;
        else if (ch == 'M')
            retval *= ui64_1024 * ui64_1024;
        else if (ch == 'G')
            retval *= ui64_1024 * ui64_1024 * ui64_1024;
        else if (ch == 'T')
            retval *= ui64_1024 * ui64_1024 * ui64_1024 * ui64_1024;
    } // if

    return retval;
} // file_size_from_string


// forward declare this for recursive magic...
static GuiOptions *build_gui_options(lua_State *L, GuiOptions *parent);

// An option table (from Setup.Option{} or Setup.OptionGroup{}) must be at
//  the top of the Lua stack.
static GuiOptions *build_one_gui_option(lua_State *L, GuiOptions *opts,
                                        boolean is_option_group)
{
    GuiOptions *newopt = NULL;
    boolean required = false;
    boolean skipopt = false;

    lua_getfield(L, -1, "required");
    if (lua_toboolean(L, -1))
    {
        lua_pushboolean(L, true);
        lua_setfield(L, -3, "value");
        required = skipopt = true;  // don't pass to GUI.
    } // if
    lua_pop(L, 1);  // remove "required" from stack.

    // "disabled=true" trumps "required=true"
    lua_getfield(L, -1, "disabled");
    if (lua_toboolean(L, -1))
    {
        if (required)
        {
            lua_getfield(L, -2, "description");
            logWarning("Option '%s' is both required and disabled!",
                        lua_tostring(L, -1));
            lua_pop(L, 1);
        } // if
        lua_pushboolean(L, false);
        lua_setfield(L, -3, "value");
        skipopt = true;  // don't pass to GUI.
    } // if
    lua_pop(L, 1);  // remove "disabled" from stack.

    if (skipopt)  // Skip this option, but look for children in required opts.
    {
        if (required)
            newopt = build_gui_options(L, opts);
    } // if

    else  // add this option.
    {
        newopt = (GuiOptions *) xmalloc(sizeof (GuiOptions));
        newopt->is_group_parent = is_option_group;
        newopt->value = true;

        lua_getfield(L, -1, "description");
        newopt->description = xstrdup(lua_tostring(L, -1));
        lua_pop(L, 1);

        if (!is_option_group)
        {
            lua_getfield(L, -1, "value");
            newopt->value = (lua_toboolean(L, -1) ? true : false);
            lua_pop(L, 1);
            lua_getfield(L, -1, "size");
            newopt->size = file_size_from_string(lua_tostring(L, -1));
            lua_pop(L, 1);
            newopt->opaque = ((int) lua_objlen(L, 4)) + 1;
            lua_pushinteger(L, newopt->opaque);
            lua_pushvalue(L, -2);
            lua_settable(L, 4);  // position #4 is our local lookup table.
        } // if

        newopt->child = build_gui_options(L, newopt);  // look for children...
        if ((is_option_group) && (!newopt->child))  // skip empty groups.
        {
            free((void *) newopt->description);
            free(newopt);
            newopt = NULL;
        } // if
    } // else

    if (newopt != NULL)
    {
        newopt->next_sibling = opts;
        opts = newopt;  // prepend to list (we'll reverse it later...)
    } // if

    return opts;
} // build_one_gui_option


static inline GuiOptions *cleanup_gui_option_list(GuiOptions *opts,
                                                  GuiOptions *parent)
{
    const boolean is_group = ((parent) && (parent->is_group_parent));
    GuiOptions *seen_enabled = NULL;
    GuiOptions *prev = NULL;
    GuiOptions *tmp = NULL;

    while (opts != NULL)
    {
        if ((is_group) && (opts->value))
        {
            if (seen_enabled)
            {
                logWarning("Options '%s' and '%s' are both enabled in group '%s'.",
                            seen_enabled->description, opts->description,
                            parent->description);
                seen_enabled->value = false;
            } // if
            seen_enabled = opts;
        } // if

        // Reverse the linked list, since we added these backwards before...
        tmp = opts->next_sibling;
        opts->next_sibling = prev;
        prev = opts;
        opts = tmp;
    } // while

    if ((prev) && (is_group) && (!seen_enabled))
    {
        logWarning("Option group '%s' has no enabled items, choosing first ('%s').",
                    parent->description, prev->description);
        prev->value = true;
    } // if
        
    return prev;
} // cleanup_gui_option_list


// the top of the stack must be the lua table with options/optiongroups.
//  We build onto (opts) "child" field.
static GuiOptions *build_gui_options(lua_State *L, GuiOptions *parent)
{
    int i = 0;
    GuiOptions *opts = NULL;
    const struct { const char *fieldname; boolean is_group; } opttype[] =
    {
        { "options", false },
        { "optiongroups", true }
    };

    for (i = 0; i < STATICARRAYLEN(opttype); i++)
    {
        const boolean is_group = opttype[i].is_group;
        lua_getfield(L, -1, opttype[i].fieldname);
        if (!lua_isnil(L, -1))
        {
            lua_pushnil(L);  // first key for iteration...
            while (lua_next(L, -2))  // replaces key, pushes value.
            {
                opts = build_one_gui_option(L, opts, is_group);
                lua_pop(L, 1);  // remove table, keep key for next iteration.
            } // while
            opts = cleanup_gui_option_list(opts, parent);
        } // if
        lua_pop(L, 1);  // pop options/optiongroups table.
    } // for

    return opts;
} // build_gui_options


// Free the tree of C structs we generated, and update the mirrored Lua tables
//  with new values...
static void done_gui_options(lua_State *L, GuiOptions *opts)
{
    if (opts != NULL)
    {
        done_gui_options(L, opts->next_sibling);
        done_gui_options(L, opts->child);

        if (opts->opaque)
        {
            // Update Lua table for this option...
            lua_pushinteger(L, opts->opaque);
            lua_gettable(L, 4);  // #4 is our local table
            lua_pushboolean(L, opts->value);
            lua_setfield(L, -2, "value");
            lua_pop(L, 1);
        } // if

        free((void *) opts->description);
        free(opts);
    } // if
} // done_gui_options


static int luahook_gui_options(lua_State *L)
{
    // The options table is arg #1.
    const int argc = lua_gettop(L);
    const int thisstage = luaL_checkint(L, 2);
    const int maxstage = luaL_checkint(L, 3);
    const boolean can_go_back = canGoBack(thisstage);
    const boolean can_go_fwd = canGoForward(thisstage, maxstage);
    boolean rc = true;
    GuiOptions *opts = NULL;

    assert(argc == 3);

    lua_newtable(L);  // we'll use this for updating the tree later.

    // Now we need to build a tree of C structs from the hierarchical table
    //  we got from Lua...
    lua_pushvalue(L, 1);  // get the Lua table onto the top of the stack...
    opts = build_gui_options(L, NULL);
    lua_pop(L, 1);  // pop the Lua table off the top of the stack...

    if (opts != NULL)  // if nothing to do, we'll go directly to next stage.
        rc = GGui->options(opts, can_go_back, can_go_fwd);

    done_gui_options(L, opts);  // free C structs, update Lua tables...
    lua_pop(L, 1);  // pop table we created.

    lua_pushboolean(L, rc);
    return 1;  // returns one boolean value.
} // luahook_gui_options


static int luahook_gui_destination(lua_State *L)
{
    const int thisstage = luaL_checkinteger(L, 2);
    const int maxstage = luaL_checkinteger(L, 3);
    const boolean can_go_back = canGoBack(thisstage);
    const boolean can_go_fwd = canGoForward(thisstage, maxstage);
    char **recommend = NULL;
    size_t reccount = 0;
    char *rc = NULL;

    if (lua_istable(L, 1))
    {
        int i;

        reccount = lua_objlen(L, 1);
        recommend = (char **) alloca(reccount * sizeof (char *));
        for (i = 0; i < reccount; i++)
        {
            const char *str = NULL;
            lua_pushinteger(L, i+1);
            lua_gettable(L, 1);
            str = lua_tostring(L, -1);
            recommend[i] = (char *) alloca(strlen(str) + 1);
            strcpy(recommend[i], str);
            lua_pop(L, 1);
        } // for
    } // if

    rc = GGui->destination((const char **) recommend, reccount,
                            can_go_back, can_go_fwd);
    if (rc == NULL)
        lua_pushnil(L);
    else
    {
        lua_pushstring(L, rc);
        free(rc);
    } // else
    return 1;
} // luahook_gui_destination


// Sets t[sym]=f, where t is on the top of the Lua stack.
static inline void set_cfunc(lua_State *L, lua_CFunction f, const char *sym)
{
    lua_pushcfunction(luaState, f);
    lua_setfield(luaState, -2, sym);
} // set_cfunc


// Sets t[sym]=f, where t is on the top of the Lua stack.
static inline void set_string(lua_State *L, const char *str, const char *sym)
{
    lua_pushstring(luaState, str);
    lua_setfield(luaState, -2, sym);
} // set_string

static inline void set_string_array(lua_State *L, int argc, const char **argv,
                                    const char *sym)
{
    int i;
    lua_newtable(luaState);
    for (i = 0; i < argc; i++)
    {
        lua_pushinteger(luaState, i+1);  // lua is option base 1!
        lua_pushstring(luaState, argv[i]);
        lua_settable(luaState, -3);
    } // for
    lua_setfield(luaState, -2, sym);
} // set_string_array


void MojoLua_setString(const char *str, const char *sym)
{
    lua_getglobal(luaState, MOJOSETUP_NAMESPACE);
    set_string(luaState, str, sym);
    lua_pop(luaState, 1);
} // MojoLua_setString


void MojoLua_setStringArray(int argc, const char **argv, const char *sym)
{
    lua_getglobal(luaState, MOJOSETUP_NAMESPACE);
    set_string_array(luaState, argc, argv, sym);
    lua_pop(luaState, 1);
} // MojoLua_setStringArray


boolean MojoLua_initLua(void)
{
    const char *envr = cmdlinestr("locale", "MOJOSETUP_LOCALE", NULL);
    char locale[16];
    char ostype[64];
    char osversion[64];

    if (envr != NULL)
        xstrncpy(locale, envr, sizeof (locale));
    else if (!MojoPlatform_locale(locale, sizeof (locale)))
        xstrncpy(locale, "???", sizeof (locale));

    if (!MojoPlatform_osType(ostype, sizeof (ostype)))
        xstrncpy(ostype, "???", sizeof (ostype));
    if (!MojoPlatform_osVersion(osversion, sizeof (osversion)))
        xstrncpy(osversion, "???", sizeof (osversion));

    assert(luaState == NULL);

    luaState = lua_newstate(MojoLua_alloc, NULL);
    if (luaState == NULL)
        return false;

    lua_atpanic(luaState, luahook_fatal);

    if (!lua_checkstack(luaState, 20))  // Just in case.
    {
        lua_close(luaState);
        luaState = NULL;
        return false;
    } // if

    luaL_openlibs(luaState);

    // Build MojoSetup namespace for Lua to access and fill in C bridges...
    lua_newtable(luaState);
        // Set up initial C functions, etc we want to expose to Lua code...
        set_cfunc(luaState, luahook_runfile, "runfile");
        set_cfunc(luaState, luahook_translate, "translate");
        set_cfunc(luaState, luahook_ticks, "ticks");
        set_cfunc(luaState, luahook_fatal, "fatal");
        set_cfunc(luaState, luahook_msgbox, "msgbox");
        set_cfunc(luaState, luahook_promptyn, "promptyn");
        set_cfunc(luaState, luahook_stackwalk, "stackwalk");
        set_cfunc(luaState, luahook_logwarning, "logwarning");
        set_cfunc(luaState, luahook_logerror, "logerror");
        set_cfunc(luaState, luahook_loginfo, "loginfo");
        set_cfunc(luaState, luahook_logdebug, "logdebug");
        set_cfunc(luaState, luahook_cmdline, "cmdline");
        set_cfunc(luaState, luahook_cmdlinestr, "cmdlinestr");
        set_cfunc(luaState, luahook_collectgarbage, "collectgarbage");
        set_cfunc(luaState, luahook_debugger, "debugger");
        set_string(luaState, locale, "locale");
        set_string(luaState, PLATFORM_NAME, "platform");
        set_string(luaState, PLATFORM_ARCH, "arch");
        set_string(luaState, ostype, "ostype");
        set_string(luaState, osversion, "osversion");
        set_string(luaState, GGui->name(), "ui");
        set_string(luaState, GBuildVer, "buildver");
        set_string(luaState, GLuaLicense, "lualicense");
        set_string_array(luaState, GArgc, GArgv, "argv");

        // Set the GUI functions...
        lua_newtable(luaState);
            set_cfunc(luaState, luahook_gui_start, "start");
            set_cfunc(luaState, luahook_gui_readme, "readme");
            set_cfunc(luaState, luahook_gui_options, "options");
            set_cfunc(luaState, luahook_gui_destination, "destination");
            set_cfunc(luaState, luahook_gui_stop, "stop");
        lua_setfield(luaState, -2, "gui");
    lua_setglobal(luaState, MOJOSETUP_NAMESPACE);

    // Set up localization table, if possible.
    MojoLua_runFile("localization");

    // Transfer control to Lua to setup some APIs and state...
    if (!MojoLua_runFile("mojosetup_init"))
        return false;

    // ...and run the installer-specific config file.
    if (!MojoLua_runFile("config"))
        return false;

    // We don't need the "Setup" namespace anymore. Make it
    //  eligible for garbage collection.
    lua_pushnil(luaState);
    lua_setglobal(luaState, "Setup");

    MojoLua_collectGarbage();  // get rid of old init crap we don't need.

    return true;
} // MojoLua_initLua


boolean MojoLua_initialized(void)
{
    return (luaState != NULL);
} // MojoLua_initialized


void MojoLua_deinitLua(void)
{
    if (luaState != NULL)
    {
        lua_close(luaState);
        luaState = NULL;
    } // if
} // MojoLua_deinitLua


const char *GLuaLicense =
"Lua:\n"
"\n"
"Copyright (C) 1994-2006 Lua.org, PUC-Rio.\n"
"\n"
"Permission is hereby granted, free of charge, to any person obtaining a copy\n"
"of this software and associated documentation files (the \"Software\"), to deal\n"
"in the Software without restriction, including without limitation the rights\n"
"to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
"copies of the Software, and to permit persons to whom the Software is\n"
"furnished to do so, subject to the following conditions:\n"
"\n"
"The above copyright notice and this permission notice shall be included in\n"
"all copies or substantial portions of the Software.\n"
"\n"
"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
"IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
"FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE\n"
"AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
"LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
"OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN\n"
"THE SOFTWARE.\n"
"\n";

// end of lua_glue.c ...

