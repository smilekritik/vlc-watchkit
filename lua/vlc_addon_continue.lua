function descriptor()
    return {
        title = "WatchKit Continue Watching",
        description = "Open WatchKit Continue Watching",
        capabilities = {"trigger"}
    }
end

function trigger()
    local ok, result = pcall(vlc.var.set, vlc.object.libvlc(), "vlc-addon-menu-command", 3)
    if not ok or (type(result) == "number" and result ~= 0) then
        vlc.msg.warn("WatchKit is unavailable. Check the installation and restart VLC.")
    end
end
