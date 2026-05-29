let music = null;
let developerToken = "";
let storefront = "ca";
let nowPlayingTimer = null;

const $ = id => document.getElementById(id);

const STORAGE_KEY = "fh6_apple_helper_settings";

function saveSettings() {
    const settings = {
        developerToken: $("token")?.value?.trim() || "",
        storefront: $("storefront")?.value?.trim() || "ca"
    };

    localStorage.setItem(STORAGE_KEY, JSON.stringify(settings));
}

function loadSettings() {
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        if (!raw) return;

        const settings = JSON.parse(raw);

        if (settings.developerToken && $("token")) {
            $("token").value = settings.developerToken;
        }

        if (settings.storefront && $("storefront")) {
            $("storefront").value = settings.storefront;
        }
    } catch {
        localStorage.removeItem(STORAGE_KEY);
    }
}

function clearSettings() {
    localStorage.removeItem(STORAGE_KEY);

    if ($("token")) {
        $("token").value = "";
    }

    if ($("storefront")) {
        $("storefront").value = "ca";
    }

    setStatus("Saved settings cleared.", "success");
}

function toast(message, type = "info", timeout = 3500) {
    const wrap = $("toasts");

    if (!wrap) {
        console.log(`[${type}] ${message}`);
        return;
    }

    const el = document.createElement("div");
    el.className = `toast ${type}`;
    el.textContent = message;

    wrap.appendChild(el);

    requestAnimationFrame(() => {
        el.classList.add("show");
    });

    setTimeout(() => {
        el.classList.remove("show");

        setTimeout(() => {
            el.remove();
        }, 200);
    }, timeout);
}

function setStatus(text, type = "info") {
    const status = $("status");

    if (status) {
        status.textContent = text;
    }

    if (text) {
        toast(text, type);
    }
}

function escapeHtml(value) {
    return String(value ?? "")
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#039;");
}

function artworkUrl(artwork, size = 300) {
    if (!artwork?.url) return "";

    return artwork.url
        .replace("{w}", String(size))
        .replace("{h}", String(size));
}

async function configureMusicKit() {
    developerToken = $("token").value.trim();
    storefront = $("storefront").value.trim() || "ca";

    saveSettings();

    if (!developerToken) {
        throw new Error("Developer token is required.");
    }

    if (!window.MusicKit) {
        throw new Error("MusicKit JS did not load.");
    }

    await MusicKit.configure({
        developerToken,
        app: {
            name: "FH6 Apple Helper",
            build: "0.2.0"
        }
    });

    music = MusicKit.getInstance();

    setStatus("MusicKit configured.", "success");
}

async function ensureMusic() {
    if (!music) {
        await configureMusicKit();
    }

    return music;
}

async function authorize() {
    const m = await ensureMusic();

    await m.authorize();

    startNowPlayingTimer();
    setStatus("Authorized.", "success");
}

async function appleMusicApi(path, params = {}, needsUserToken = false) {
    const m = await ensureMusic();

    const url = new URL(`https://api.music.apple.com${path}`);

    for (const [key, value] of Object.entries(params)) {
        if (value !== undefined && value !== null && value !== "") {
            url.searchParams.set(key, String(value));
        }
    }

    const headers = {
        Authorization: `Bearer ${developerToken}`
    };

    if (needsUserToken) {
        if (!m.musicUserToken) {
            throw new Error("Authorize Apple Music first.");
        }

        headers["Music-User-Token"] = m.musicUserToken;
    }

    const res = await fetch(url.toString(), { headers });

    if (!res.ok) {
        throw new Error(`Apple Music API error ${res.status}: ${await res.text()}`);
    }

    return await res.json();
}

async function searchCatalog(query) {
    const result = await appleMusicApi(
        `/v1/catalog/${storefront}/search`,
        {
            term: query,
            types: "songs,albums,playlists",
            limit: 15
        },
        false
    );

    return {
        songs: result?.results?.songs?.data || [],
        albums: result?.results?.albums?.data || [],
        playlists: result?.results?.playlists?.data || []
    };
}

async function searchLibrary(query) {
    const result = await appleMusicApi(
        "/v1/me/library/search",
        {
            term: query,
            types: "library-songs,library-albums,library-playlists",
            limit: 15
        },
        true
    );

    return {
        songs: result?.results?.["library-songs"]?.data || [],
        albums: result?.results?.["library-albums"]?.data || [],
        playlists: result?.results?.["library-playlists"]?.data || []
    };
}

async function loadLibraryPlaylists() {
    const result = await appleMusicApi(
        "/v1/me/library/playlists",
        {
            limit: 100
        },
        true
    );

    return result?.data || [];
}

async function loadLibraryPlaylistTracks(playlistId) {
    const result = await appleMusicApi(
        `/v1/me/library/playlists/${encodeURIComponent(playlistId)}/tracks`,
        {
            limit: 100
        },
        true
    );

    return result?.data || [];
}

async function playCatalogSong(song) {
    const m = await ensureMusic();

    await m.setQueue({ song: song.id });
    await m.play();

    startNowPlayingTimer();
    updateNowPlaying();

    setStatus(`Playing: ${song.attributes?.name || song.id}`, "success");
}

async function playCatalogAlbum(album) {
    const m = await ensureMusic();

    await m.setQueue({ album: album.id });
    await m.play();

    startNowPlayingTimer();
    updateNowPlaying();

    setStatus(`Playing album: ${album.attributes?.name || album.id}`, "success");
}

async function playCatalogPlaylist(playlist) {
    const m = await ensureMusic();

    await m.setQueue({ playlist: playlist.id });
    await m.play();

    startNowPlayingTimer();
    updateNowPlaying();

    setStatus(`Playing playlist: ${playlist.attributes?.name || playlist.id}`, "success");
}

async function playLibraryItem(item) {
    const m = await ensureMusic();

    await m.setQueue({ items: [item] });
    await m.play();

    startNowPlayingTimer();
    updateNowPlaying();

    setStatus(
        `Playing: ${item.attributes?.name || item.attributes?.title || item.id}`,
        "success"
    );
}

async function playLibraryPlaylist(playlist) {
    const tracks = await loadLibraryPlaylistTracks(playlist.id);

    if (!tracks.length) {
        throw new Error("This playlist has no playable tracks.");
    }

    const m = await ensureMusic();

    await m.setQueue({ items: tracks });
    await m.play();

    startNowPlayingTimer();
    updateNowPlaying();

    setStatus(`Playing playlist: ${playlist.attributes?.name || playlist.id}`, "success");
}

function renderSection(container, title, items, onClick) {
    if (!items.length) return;

    const heading = document.createElement("h3");
    heading.textContent = title;
    container.appendChild(heading);

    for (const item of items) {
        const attrs = item.attributes || {};
        const name = attrs.name || attrs.title || "Unknown";
        const artist = attrs.artistName || attrs.curatorName || "";
        const album = attrs.albumName || "";

        const btn = document.createElement("button");
        btn.className = "result";
        btn.innerHTML = `
      <strong>${escapeHtml(name)}</strong><br>
      <span class="muted">${escapeHtml([artist, album].filter(Boolean).join(" - "))}</span>
    `;

        btn.addEventListener("click", async () => {
            try {
                await onClick(item);
            } catch (err) {
                setStatus(err.message, "error");
            }
        });

        container.appendChild(btn);
    }
}

function renderCatalogResults(results) {
    const wrap = $("search-results");
    wrap.innerHTML = "";

    renderSection(wrap, "Songs", results.songs, playCatalogSong);
    renderSection(wrap, "Albums", results.albums, playCatalogAlbum);
    renderSection(wrap, "Playlists", results.playlists, playCatalogPlaylist);

    if (!wrap.innerHTML.trim()) {
        wrap.innerHTML = `<p class="muted">No catalog results found.</p>`;
    }
}

function renderLibraryResults(results) {
    const wrap = $("search-results");
    wrap.innerHTML = "";

    renderSection(wrap, "Library Songs", results.songs, playLibraryItem);
    renderSection(wrap, "Library Albums", results.albums, playLibraryItem);
    renderSection(wrap, "Library Playlists", results.playlists, async playlist => {
        await renderPlaylistTracks(playlist);
    });

    if (!wrap.innerHTML.trim()) {
        wrap.innerHTML = `<p class="muted">No library results found.</p>`;
    }
}

async function renderPlaylists() {
    const playlists = await loadLibraryPlaylists();

    const wrap = $("playlists");
    wrap.innerHTML = "";

    if (!playlists.length) {
        wrap.innerHTML = `<p class="muted">No playlists found.</p>`;
        setStatus("No playlists found.", "info");
        return;
    }

    for (const playlist of playlists) {
        const attrs = playlist.attributes || {};
        const btn = document.createElement("button");

        btn.className = "result";
        btn.innerHTML = `
      <strong>${escapeHtml(attrs.name || "Untitled Playlist")}</strong><br>
      <span class="muted">${escapeHtml(attrs.description?.standard || "")}</span>
    `;

        btn.addEventListener("click", async () => {
            try {
                await renderPlaylistTracks(playlist);
            } catch (err) {
                setStatus(err.message, "error");
            }
        });

        wrap.appendChild(btn);
    }

    setStatus(`Loaded ${playlists.length} playlist(s).`, "success");
}

async function renderPlaylistTracks(playlist) {
    const tracks = await loadLibraryPlaylistTracks(playlist.id);

    const wrap = $("playlist-tracks");
    wrap.innerHTML = "";

    const title = document.createElement("h4");
    title.textContent = playlist.attributes?.name || "Playlist";
    wrap.appendChild(title);

    const playAll = document.createElement("button");
    playAll.className = "result";
    playAll.innerHTML = `<strong>Play entire playlist</strong>`;
    playAll.addEventListener("click", async () => {
        try {
            await playLibraryPlaylist(playlist);
        } catch (err) {
            setStatus(err.message, "error");
        }
    });
    wrap.appendChild(playAll);

    if (!tracks.length) {
        wrap.innerHTML += `<p class="muted">No tracks found.</p>`;
        setStatus("No tracks found in playlist.", "info");
        return;
    }

    for (const track of tracks) {
        const attrs = track.attributes || {};
        const btn = document.createElement("button");

        btn.className = "result";
        btn.innerHTML = `
      <strong>${escapeHtml(attrs.name || attrs.title || "Unknown")}</strong><br>
      <span class="muted">${escapeHtml([attrs.artistName, attrs.albumName].filter(Boolean).join(" - "))}</span>
    `;

        btn.addEventListener("click", async () => {
            try {
                await playLibraryItem(track);
            } catch (err) {
                setStatus(err.message, "error");
            }
        });

        wrap.appendChild(btn);
    }

    setStatus(`Loaded ${tracks.length} track(s).`, "success");
}

function updateNowPlaying() {
    if (!music) return;

    const item = music.nowPlayingItem;
    const attrs = item?.attributes || {};

    const title = attrs.name || attrs.title || "Nothing playing";
    const artist = attrs.artistName || "";
    const album = attrs.albumName || "";
    const art = artworkUrl(attrs.artwork, 300);

    $("now-title").textContent = title;
    $("now-artist").textContent = artist;
    $("now-album").textContent = album;

    if (art) {
        $("now-artwork").src = art;
        $("now-artwork").style.display = "block";
    } else {
        $("now-artwork").removeAttribute("src");
    }
}

function startNowPlayingTimer() {
    if (nowPlayingTimer) return;

    nowPlayingTimer = setInterval(() => {
        updateNowPlaying();
    }, 1000);
}

$("configure").addEventListener("click", async () => {
    try {
        await configureMusicKit();
    } catch (err) {
        setStatus(err.message, "error");
    }
});

$("authorize").addEventListener("click", async () => {
    try {
        await authorize();
    } catch (err) {
        setStatus(err.message, "error");
    }
});

$("capture-test").addEventListener("click", () => {
    setStatus("Capture test requested.", "info");

    if (window.chrome?.webview) {
        window.chrome.webview.postMessage(JSON.stringify({
            type: "capture-test",
            seconds: 10
        }));
    } else {
        setStatus("WebView2 host messaging is not available.", "error");
    }
});

$("start-stream").addEventListener("click", () => {
    setStatus("Starting FH6 stream...", "info");

    if (window.chrome?.webview) {
        window.chrome.webview.postMessage(JSON.stringify({
            type: "start-stream"
        }));

        setStatus("FH6 stream requested.", "success");
    } else {
        setStatus("WebView2 host messaging is not available.", "error");
    }
});

$("stop-stream").addEventListener("click", () => {
    setStatus("Stopping FH6 stream...", "info");

    if (window.chrome?.webview) {
        window.chrome.webview.postMessage(JSON.stringify({
            type: "stop-stream"
        }));

        setStatus("FH6 stream stop requested.", "success");
    } else {
        setStatus("WebView2 host messaging is not available.", "error");
    }
});

$("search-form").addEventListener("submit", async e => {
    e.preventDefault();

    const query = $("query").value.trim();
    if (!query) return;

    try {
        setStatus("Searching catalog...", "info");

        const results = await searchCatalog(query);

        renderCatalogResults(results);
        setStatus("Catalog search complete.", "success");
    } catch (err) {
        setStatus(err.message, "error");
    }
});

$("search-library").addEventListener("click", async () => {
    const query = $("query").value.trim();
    if (!query) return;

    try {
        setStatus("Searching library...", "info");

        const results = await searchLibrary(query);

        renderLibraryResults(results);
        setStatus("Library search complete.", "success");
    } catch (err) {
        setStatus(err.message, "error");
    }
});

$("load-playlists").addEventListener("click", async () => {
    try {
        setStatus("Loading playlists...", "info");

        await renderPlaylists();
    } catch (err) {
        setStatus(err.message, "error");
    }
});

$("play-pause").addEventListener("click", async () => {
    try {
        const m = await ensureMusic();

        if (m.isPlaying) {
            await m.pause();
            setStatus("Paused.", "info");
        } else {
            await m.play();
            setStatus("Playing.", "success");
        }

        updateNowPlaying();
    } catch (err) {
        setStatus(err.message, "error");
    }
});

$("next").addEventListener("click", async () => {
    try {
        const m = await ensureMusic();

        await m.skipToNextItem();
        updateNowPlaying();

        setStatus("Skipped to next track.", "success");
    } catch (err) {
        setStatus(err.message, "error");
    }
});

$("prev").addEventListener("click", async () => {
    try {
        const m = await ensureMusic();

        await m.skipToPreviousItem();
        updateNowPlaying();

        setStatus("Skipped to previous track.", "success");
    } catch (err) {
        setStatus(err.message, "error");
    }
});

$("clear-settings").addEventListener("click", () => {
    clearSettings();
});

loadSettings();