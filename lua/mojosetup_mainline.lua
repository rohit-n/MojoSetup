-- This is where most of the magic happens. Everything is initialized, and
--  the user's config script has successfully run. This Lua chunk drives the
--  main application code.

-- This is just for convenience.
local _ = MojoSetup.translate

-- This dumps the table built from the user's config script using logdebug,
--  so it only spits out crap if debug-level logging is enabled.
MojoSetup.dumptable("MojoSetup.installs", MojoSetup.installs)

local function do_install(install)
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
        local desc = eula.description;
        local fname = eula.source;

        -- (desc) and (fname) become an upvalues in this function.
        stages[#stages+1] = function (thisstage, maxstage)
            if not MojoSetup.gui.readme(desc, fname, thisstage, maxstage) then
                return false
            end

            if not MojoSetup.promptyn(desc, _("Accept this license?")) then
                MojoSetup.fatal(_("You must accept the license before you may install"));
            end
            return true
        end
    end

    -- Next stage: show any READMEs.
    for k,readme in pairs(install.readmes) do
        local desc = readme.description;
        local fname = readme.source;
        -- (desc) and (fname) become upvalues in this function.
        stages[#stages+1] = function(thisstage, maxstage)
            return MojoSetup.gui.readme(desc, fname, thisstage, maxstage)
        end
    end

    -- Next stage: let user choose install destination.
    --  The config file can force a destination if it has a really good reason
    --  (system drivers that have to go in a specific place, for example),
    --  but really really shouldn't in 99% of the cases.
    if install.destination == nil then
        local recommend = install.recommended_destinations
        -- (recommend) becomes an upvalue in this function.
        stages[#stages+1] = function(thisstage, maxstage)
            local x = MojoSetup.gui.destination(recommend, thisstage, maxstage)
            if x == nil then
                return false   -- go back
            end
            install.destination = x
            return true
        end
    end

    -- Next stage: let user choose install options.
    if install.options ~= nil or install.optiongroups ~= nil then
        local options =
        {
            options = install.options,
            optiongroups = install.optiongroups
        }

        -- (options) becomes an upvalue in this function.
        stages[#stages+1] = function(thisstage, maxstage)
            -- This does some complex stuff with a hierarchy of tables in C.
            return MojoSetup.gui.options(options, thisstage, maxstage)
        end
    end

    -- Next stage: actual installation.

    -- Next stage: show results gui
        -- On failure, back out changes (make this part of fatal()).

    -- Now make all this happen.
    if not MojoSetup.gui.start(install.description, install.splash) then
        MojoSetup.fatal(_("GUI failed to start"))
    end

    -- Make the stages available elsewhere.
    MojoSetup.stages = stages

    MojoSetup.installed_files = {}

    local i = 1
    while MojoSetup.stages[i] ~= nil do
        local stage = MojoSetup.stages[i]
        local go_forward = stage(i, #MojoSetup.stages)
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

    MojoSetup.logdebug("Install destination is '" .. install.destination .. "'")

    -- Done with this. Make it eligible for garbage collection.
    MojoSetup.stages = nil

    -- Don't let future errors delete files from successful installs...
    MojoSetup.installed_files = nil

    MojoSetup.gui.stop()
end


local saw_an_installer = false
for installkey,install in pairs(MojoSetup.installs) do
    saw_an_installer = true
    do_install(install)
end

if (not saw_an_installer) then
    MojoSetup.fatal(_("Nothing to do!"))
end

-- end of mojosetup_mainline.lua ...

