-- This is where most of the magic happens. Everything is initialized, and
--  the user's config script has successfully run. This Lua chunk drives the
--  main application code.

-- !!! FIXME: add a --dryrun option.

-- This is just for convenience.
local _ = MojoSetup.translate

-- This dumps the table built from the user's config script using logdebug,
--  so it only spits out crap if debug-level logging is enabled.
MojoSetup.dumptable("MojoSetup.installs", MojoSetup.installs)


local function split_path(path)
    local retval = {}
    for item in string.gmatch(path .. "/", "(.-)/") do
        if item ~= "" then
            retval[#retval+1] = item
        end
    end
    return retval
end

local function rebuild_path(paths, n)
    local retval = paths[n]
    n = n + 1
    while paths[n] ~= nil do
        retval = retval .. "/" .. paths[n]
        n = n + 1
    end
    return retval
end

local function normalize_path(path)
    return rebuild_path(split_path(path), 1)
end


local function is_base_url(src)
    local retval = true
    if src ~= nil then
        local prot,host,path = MojoSetup.spliturl(file.source)
        if prot ~= "base://" then
            retval = false
        end
    end
    return retval
end


-- This code's a little nasty...
local drill_for_archive = function(archive, path)
    if not MojoSetup.archive.enumerate(archive, nil) then
        MojoSetup.fatal(_("Couldn't enumerate archive."))  -- !!! FIXME: error message
    end

    local pathtab = split_path(path)
    local ent = MojoSetup.archive.enumnext(archive)
    while ent ~= nil do
        if ent.type == "file" then
            local i = 1
            local enttab = split_path(ent.filename)
            while (enttab[i] ~= nil) and (enttab[i] == pathtab[i]) do
                i = i + 1
            end

            if enttab[i] == nil then
                -- It's a file that makes up some of the specified path.
                --  open it as an archive and keep drilling...
                local arc = MojoSetup.archive.fromentry(archive)
                if arc == nil then
                    MojoSetup.fatal(_("Couldn't open archive."))  -- !!! FIXME: error message.
                end
                if pathtab[i] == nil then
                    return arc  -- this is the end of the path! Done drilling!
                end
                local retval = drill_for_archive(arc, rebuild_path(paths, i))
                MojoSetup.archive.close(arc)
                return retval
            end
        end
    end

    MojoSetup.fatal(_("Archive not found."))  -- !!! FIXME: error message.
end


local function delete_files(filelist, callback, error_is_fatal)
    local max = #filelist
    for i,v in ipairs(filelist) do
        if not MojoSetup.platform.unlink(v) and error_is_fatal then
            -- !!! FIXME: formatting
            MojoSetup.fatal(_("Deletion failed!"))
        end
        MojoSetup.loginfo("Deleted '" .. v .. "'")

        if callback ~= nil then
            callback(i, max)
        end
    end
end


local function install_file(path, archive, file)
    if not MojoSetup.writefile(archive, path) then
        -- !!! FIXME: formatting!
        MojoSetup.fatal(_("file creation failed!"))
    end
    MojoSetup.loginfo("Created file '" .. path .. "'")
    MojoSetup.installed_files[#MojoSetup.installed_files+1] = path
end


local function install_symlink(path, lndest)
    if not MojoSetup.platform.symlink(path, lndest) then
        -- !!! FIXME: formatting!
        MojoSetup.fatal(_("symlink creation failed!"))
    end
    MojoSetup.loginfo("Created symlink '" .. path .. "' -> '" .. lndest .. "'")
    MojoSetup.installed_files[#MojoSetup.installed_files+1] = path
end


local function install_directory(path)
    if not MojoSetup.platform.mkdir(path) then
        -- !!! FIXME: formatting
        MojoSetup.fatal(_("mkdir failed"))
    end
    MojoSetup.loginfo("Created directory '" .. path .. "'")
    MojoSetup.installed_files[#MojoSetup.installed_files+1] = path
end


local function install_parent_dirs(path)
    -- Chop any '/' chars from the end of the string...
    path = string.gsub(path, "/+$", "")

    -- Build each piece of final path. The gmatch() skips the last element.
    local fullpath = ""
    for item in string.gmatch(path, "(.-)/") do
        if item ~= "" then
            fullpath = fullpath .. "/" .. item
            if not MojoSetup.platform.exists(fullpath) then
                install_directory(fullpath)
            end
        end
    end
end


local function permit_write(dest, entinfo, file)
    local allowoverwrite = true
    if MojoSetup.platform.exists(dest) then
MojoSetup.logdebug(dest .. " exists, entinfo.type is '" .. entinfo.type .. "'")
        -- never "permit" existing dirs, so they don't rollback.
        if entinfo.type == "dir" then
            allowoverwrite = false
        else
            allowoverwrite = file.allowoverwrite
            if not allowoverwrite then
                -- !!! FIXME: language and formatting.
                MojoSetup.logdebug("File '" .. dest .. "' already exists.")
                allowoverwrite = MojoSetup.promptyn(_("Conflict!"), _("File already exists! Replace?"))
            end

            if allowoverwrite then
                local id = #MojoSetup.rollbacks + 1
                -- !!! FIXME: dirname may clash.
                local f = MojoSetup.destination .. "/.mojosetup_tmp/rollback"
                f = f .. "/" .. id
                -- !!! FIXME: don't add (f) to the installed_files table...
                install_parent_dirs(f)
                if not MojoSetup.movefile(dest, f) then
                    -- !!! FIXME: formatting
                    MojoSetup.fatal(_("Couldn't backup file for rollback"))
                end
                MojoSetup.rollbacks[id] = dest
                MojoSetup.loginfo("Moved rollback #" .. id .. ": '" .. dest .. "' -> '" .. f .. "'")
            end
        end
    end

    return allowoverwrite
end


local function install_archive_entry(archive, ent, file)
    local entdest = nil
    if ent.basepath ~= nil then
        entdest = ent.basepath .. "/" .. ent.filename
    else
        entdest = ent.filename
    end
    if entdest == nil then return end   -- probably can't happen...

    -- Set destination in native filesystem. May be default or explicit.
    local dest = file.destination
    if dest == nil then
        dest = entdest
    else
        entdest = string.gsub(entdest, "^.*/", "", 1)
        dest = dest .. "/" .. entdest
    end

    if file.filter ~= nil then
        dest = file.filter(dest)
    end

    if dest ~= nil then  -- Only install if file wasn't filtered out
        dest = MojoSetup.destination .. "/" .. dest
        if permit_write(dest, ent, file) then
            install_parent_dirs(dest)
            if ent.type == "file" then
                install_file(dest, archive, file)
            elseif ent.type == "dir" then
                install_directory(dest)
            elseif ent.type == "symlink" then
                install_symlink(dest, ent.linkdest)
            else  -- !!! FIXME: device nodes, etc...
                -- !!! FIXME: formatting!
                -- !!! FIXME: should this be fatal?
                MojoSetup.fatal(_("Unknown file type in archive"))
            end
        end
    end
end


local function install_archive(archive, file, option)
    if not MojoSetup.archive.enumerate(archive, nil) then
        -- !!! FIXME: need formatting function.
        MojoSetup.fatal(_("Can't enumerate archive"))
    end

    local isbase = is_base_url(file.source)
    local single_match = true
    local wildcards = file.wildcards

    if wildcards ~= nil then
        if type(wildcards) == "string" then
            wildcards = { wildcards }
        end
        for k,v in ipairs(wildcards) do
            if string.find(v, "[*?]") ~= nil then
                single_match = false
                break  -- no reason to keep iterating...
            end
        end
    end

    local ent = MojoSetup.archive.enumnext(archive)
    while ent ~= nil do
        -- If inside GBaseArchive (no URL lead in string), then we
        --  want to clip to data/ directory...
        if isbase then
            local count
            ent.filename, count = string.gsub(ent.filename, "^data/", "", 1)
            if count == 0 then
                ent.filename = nil
            end
        end
        
        -- See if we should install this file...
        if ent.filename ~= nil then
            local should_install = false
            if wildcards == nil then
                should_install = true
            else
                for k,v in ipairs(wildcards) do
                    if MojoSetup.wildcardmatch(ent.filename, v) then
                        should_install = true
                        break  -- no reason to keep iterating...
                    end
                end
            end

            if should_install then
                install_archive_entry(archive, ent, file)
                if single_match then
                    break   -- no sense in iterating further if we're done.
                end
            end
        end

        -- and check the next entry in the archive...
        ent = MojoSetup.archive.enumnext(archive)
    end
end


local function install_basepath(basepath, file, option)
    -- Obviously, we don't want to enumerate the entire physical filesystem,
    --  so we'll dig through each path element with MojoPlatform_exists()
    --  until we find one that doesn't, then we'll back up and try to open
    --  that as a directory, and then a file archive, and start drilling from
    --  there. Fun.
    local paths = split_path(basepath)
    local path = ""
    for i,v in ipairs(paths) do
        local knowngood = path
        path = path .. "/" .. v
        if not MojoSetup.platform.exists(path) then
            if knowngood == "" then  -- !!! FIXME: error message.
                MojoSetup.fatal(_("archive not found"))
            end
            local archive = MojoSetup.archive.fromdir(knowngood)
            if archive == nil then
                archive = MojoSetup.archive.fromfile(knowngood)
                if archive == nil then  -- !!! FIXME: error message.
                    MojoSetup.fatal(_("cannot open archive."))
                end
            end
            local arc = drill_for_archive(archive, rebuild_path(paths, i))
            MojoSetup.archive.close(archive)
            install_archive(arc, file, option)
            MojoSetup.archive.close(arc)
        end
    end
end


local function do_install(install)
    -- !!! FIXME: try to sanity check everything we can here
    -- !!! FIXME:  (unsupported URLs, bogus media IDs, etc.)
    -- !!! FIXME:  I would like everything possible to fail here instead of
    -- !!! FIXME:  when a user happens to pick an option no one tested...

    -- This is to save us the trouble of a loop every time we have to
    --  find media by id...
    MojoSetup.media = {}
    if install.medias ~= nil then
        for k,v in pairs(install.medias) do
            if MojoSetup.media[v.id] ~= nil then
                MojoSetup.fatal(_("Config bug: duplicate media id"))
            end
            MojoSetup.media[v.id] = v
        end
    end

    -- Build a bunch of functions into a linear array...this lets us move
    --  back and forth between stages of the install with customized functions
    --  for each bit that have their own unique params embedded as upvalues.
    -- So if there are three EULAs to accept, we'll call three variations of
    --  the EULA function with three different tables that appear as local
    --  variables, and the driver that calls this function will be able to
    --  skip back and forth based on user input. This is a cool Lua thing.
    local stages = {}

    -- First stage: Make sure installer can run. Always fails or steps forward.
    if install.precheck ~= nil then
        stages[#stages+1] = function ()
            MojoSetup.gui.precheck()
            local errstr = install.precheck()
            if errstr ~= nil then
                MojoSetup.fatal(errstr)
            end
        end
    end

    -- Next stage: accept all EULAs. Never lets user step back, so they
    --  either accept or reject and go to the next EULA or stage. Rejection
    --  of any EULA is considered fatal.
    for k,eula in pairs(install.eulas) do
        local desc = eula.description
        local fname = "data/" .. eula.source

        -- (desc) and (fname) become an upvalues in this function.
        stages[#stages+1] = function (thisstage, maxstage)
            if not MojoSetup.gui.readme(desc, fname, thisstage, maxstage) then
                return false
            end

            if not MojoSetup.promptyn(desc, _("Accept this license?")) then
                MojoSetup.fatal(_("You must accept the license before you may install"))
            end
            return true
        end
    end

    -- Next stage: show any READMEs.
    for k,readme in pairs(install.readmes) do
        local desc = readme.description
        local fname = "data/" .. readme.source
        -- (desc) and (fname) become upvalues in this function.
        stages[#stages+1] = function(thisstage, maxstage)
            return MojoSetup.gui.readme(desc, fname, thisstage, maxstage)
        end
    end

    -- Next stage: let user choose install destination.
    --  The config file can force a destination if it has a really good reason
    --  (system drivers that have to go in a specific place, for example),
    --  but really really shouldn't in 99% of the cases.
    MojoSetup.destination = install.destination
    if MojoSetup.destination ~= nil then
        MojoSetup.logdebug("Install dest: '" .. install.destination .. "'")
    else
        local recommend = install.recommended_destinations
        -- (recommend) becomes an upvalue in this function.
        stages[#stages+1] = function(thisstage, maxstage)
            local x = MojoSetup.gui.destination(recommend, thisstage, maxstage)
            if x == nil then
                return false   -- go back
            end
            MojoSetup.destination = x
            MojoSetup.logdebug("Install dest: '" .. x .. "'")
            return true
        end
    end

    -- Next stage: let user choose install options.
    if install.options ~= nil or install.optiongroups ~= nil then
        -- (install) becomes an upvalue in this function.
        stages[#stages+1] = function(thisstage, maxstage)
            -- This does some complex stuff with a hierarchy of tables in C.
            return MojoSetup.gui.options(install, thisstage, maxstage)
        end
    end


    -- Next stage: Make sure source list is sane.
    --  This is not a GUI stage, it just needs to run between them.
    --  This gets a little hairy.
    stages[#stages+1] = function(thisstage, maxstage)
        local function process_file(option, file)
            -- !!! FIXME: what happens if a file shows up in multiple options?
            local src = file.source
            local prot,host,path
            if src ~= nil then  -- no source, it's in GBaseArchive
                prot,host,path = MojoSetup.spliturl(src)
            end
            if (src == nil) or (prot == "base://") then  -- included content?
                if MojoSetup.files.included == nil then
                    MojoSetup.files.included = {}
                end
                MojoSetup.files.included[file] = option
            elseif prot == "media://" then
                -- !!! FIXME: make sure media id is valid.
                if MojoSetup.files.media == nil then
                    MojoSetup.files.media = {}
                end
                if MojoSetup.files.media[host] == nil then
                    MojoSetup.files.media[host] = {}
                end
                MojoSetup.files.media[host][file] = option
            else
                -- !!! FIXME: make sure we can handle this URL...
                if MojoSetup.files.downloads == nil then
                    MojoSetup.files.downloads = {}
                end
                MojoSetup.files.downloads[file] = option
            end
        end

        -- Sort an option into tables that say what sort of install it is.
        --  This lets us batch all the things from one CD together,
        --  do all the downloads first, etc.
        local function process_option(option)
            -- !!! FIXME: check for nil in schema?
            if option.files ~= nil then
                for k,v in pairs(option.files) do
                    process_file(option, v)
                end
            end
        end

        local build_source_tables = function(opts)
            local options = opts['options']
            if options ~= nil then
                for k,v in pairs(options) do
                    if v.value then
                        process_option(v)
                    end
                end
            end

            local optiongroups = opts['optiongroups']
            if optiongroups ~= nil then
                for k,v in pairs(optiongroups) do
                    if not v.disabled then
                        build_source_tables(optiongroups)
                    end
                end
            end
        end

        MojoSetup.files = {}
        build_source_tables(install)

        -- This dumps the files tables using MojoSetup.logdebug,
        --  so it only spits out crap if debug-level logging is enabled.
        MojoSetup.dumptable("MojoSetup.files", MojoSetup.files)

        return true   -- always go forward from non-GUI stage.
    end

    -- Next stage: Download external packages.
    stages[#stages+1] = function(thisstage, maxstage)
        if MojoSetup.files.downloads ~= nil then
            MojoSetup.gui.startdownload()
            -- !!! FIXME: write me.
            MojoSetup.gui.enddownload()
        end
        return true
    end

    -- Next stage: actual installation.
    stages[#stages+1] = function(thisstage, maxstage)
        MojoSetup.gui.startinstall()
        -- Do stuff on media first, so the user finds out he's missing
        --  disc 3 of 57 as soon as possible...

        -- Since we sort all things to install by the media that contains
        --  the source data, you should only have to insert each disc
        --  once, no matter how they landed in the config file.

        if MojoSetup.files.media ~= nil then
            -- Build an array of media ids so we can sort them into a
            --  reasonable order...disc 1 before disc 2, etc.
            local medialist = {}
            for mediaid,mediavalue in pairs(MojoSetup.files.media) do
                medialist[#medialist+1] = mediaid
            end
            table.sort(medialist)

            for mediaidx,mediaid in ipairs(medialist) do
                local media = MojoSetup.media[mediaid]
                local basepath = MojoSetup.findmedia(media.uniquefile)
                while basepath == nil do
                    if not MojoSetup.gui.insertmedia(media.description) then
                        MojoSetup.fatal(_("User cancelled."))  -- !!! FIXME: don't like this.
                    end
                    basepath = MojoSetup.findmedia(media.uniquefile)
                end

                -- Media is ready, install everything from this one...
                MojoSetup.loginfo("Found correct media at '" .. basepath .. "'")
                local files = MojoSetup.files.media[mediaid]
                for file,option in pairs(files) do
                    install_basepath(basepath, file, option)
                end
            end
            medialist = nil   -- done with this.
        end

        if MojoSetup.files.downloads ~= nil then
            for file,option in pairs(MojoSetup.files.downloads) do
                install_basepath(basepath, file, option)
            end
        end

        if MojoSetup.files.included ~= nil then
            for file,option in pairs(MojoSetup.files.included) do
                local arc = MojoSetup.archive.base
                if file.source == nil then
                    install_archive(arc, file, option)
                else
                    local prot,host,path = MojoSetup.spliturl(file.source)
                    arc = drill_for_archive(arc, path)
                    install_archive(arc, file, option)
                    MojoSetup.archive.close(arc)
                end
            end
        end

        MojoSetup.gui.endinstall()
        return true   -- go to next stage.
    end

    -- Next stage: show results gui
        -- On failure, back out changes (make this part of fatal()).

    -- Now make all this happen.
    if not MojoSetup.gui.start(install.description, install.splash) then
        MojoSetup.fatal(_("GUI failed to start"))
    end

    -- Make the stages available elsewhere.
    MojoSetup.stages = stages

    MojoSetup.installed_files = {}
    MojoSetup.rollbacks = {}

    local i = 1
    while MojoSetup.stages[i] ~= nil do
        local stage = MojoSetup.stages[i]
        local go_forward = stage(i, #MojoSetup.stages)

        -- Too many times I forgot to return something.   :)
        if go_forward == nil then
            MojoSetup.fatal("Bug in the installer: stage returned nil.")
        end

        if go_forward then
            i = i + 1
        else
            if i == 1 then
                MojoSetup.logwarning("Stepped back over start of stages")
                MojoSetup.fatal(_("Internal error"))
            else
                i = i - 1
            end
        end
    end

    -- !!! FIXME: write out manifest.

    -- Successful install, so delete conflicts we no longer need to rollback.
    delete_files(MojoSetup.rollbacks)

    -- !!! FIXME: nuke scratch files (downloaded files, rollbacks, etc)...

    -- Don't let future errors delete files from successful installs...
    MojoSetup.installed_files = nil
    MojoSetup.rollbacks = nil

    MojoSetup.gui.stop()

    -- Done with these things. Make them eligible for garbage collection.
    MojoSetup.stages = nil
    MojoSetup.files = nil
    MojoSetup.media = nil
    MojoSetup.destination = nil
end

-- !!! FIXME: is MojoSetup.installs just nil? Should we lose saw_an_installer?
local saw_an_installer = false
for installkey,install in pairs(MojoSetup.installs) do
    saw_an_installer = true
    do_install(install)
    MojoSetup.collectgarbage()  -- nuke all the tables we threw around...
end

if not saw_an_installer then
    MojoSetup.fatal(_("Nothing to do!"))
end

-- end of mojosetup_mainline.lua ...

