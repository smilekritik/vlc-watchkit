function descriptor()
    return {
        title = "WatchKit Bookmarks",
        description = "Open WatchKit Bookmarks",
        capabilities = {"trigger"}
    }
end

function trigger()
    local ok, result = pcall(vlc.var.set, vlc.object.libvlc(), "vlc-addon-menu-command", 2)
    if not ok or (type(result) == "number" and result ~= 0) then
        vlc.msg.warn("WatchKit is unavailable. Check the installation and restart VLC.")
    end
end
