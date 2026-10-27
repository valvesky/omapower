.pragma library

// Match the daemon's whitelist validation; never treat invalid input as defaults.
function validName(name) {
    if (typeof name !== "string" || !name || name.indexOf("/") !== -1 || name.indexOf("\n") !== -1) return false
    try { return encodeURIComponent(name).replace(/%[0-9A-F]{2}/g, "x").length <= 15 }
    catch (e) { return false }
}

function parse(raw) {
    if (typeof raw !== "string" || raw.length > 65536) return null
    try {
        const data = JSON.parse(raw)
        if (!data || !Array.isArray(data.commands)) return null
        const result = []
        for (const name of data.commands) {
            if (!validName(name)) return null
            if (result.indexOf(name) === -1) result.push(name)
        }
        return { commands: result.sort() }
    } catch (e) { return null }
}
