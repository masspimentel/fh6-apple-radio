let music = null;
let developerToken = "";
let storefront = "ca";

const $ = id => document.getElementById(id);

function setStatus(text) {
    $("status").textContent = text;
}

function escapeHtml(value) {
    return String(value ?? "")
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#039;");
}

async function configureMusicKit() {
    developerToken = $("token").value.trim();
    storefront = $("storefront").value.trim() || "ca";

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
            build: "0.1.0"
        }
    });

    music = MusicKit.getInstance();
    setStatus("MusicKit configured.");
}

async function authorize() {
    if (!music) {
        await configureMusicKit();
    }

    await music.authorize();
    setStatus("Authorized.");
}

async function appleMusicApi(path, params = {}, needsUserToken = false) {
    if (!music) {
        await configureMusicKit();
    }

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
        if (!music.musicUserToken) {
            throw new Error("Authorize Apple Music first.");
        }

        headers["Music-User-Token"] = music.musicUserToken;
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
            types: "songs",
            limit: 10
        },
        false
    );

    return result?.results?.songs?.data || [];
}

async function playSong(song) {
    if (!music) {
        await configureMusicKit();
    }

    await music.setQueue({ song: song.id });
    await music.play();

    setStatus(`Playing: ${song.attributes?.name ?? song.id}`);
}

function renderSongs(songs) {
    const results = $("results");
    results.innerHTML = "";

    if (!songs.length) {
        results.innerHTML = `<p class="muted">No results found.</p>`;
        return;
    }

    for (const song of songs) {
        const attrs = song.attributes || {};
        const btn = document.createElement("button");
        btn.className = "result";
        btn.innerHTML = `
      <strong>${escapeHtml(attrs.name || "Unknown")}</strong><br>
      <span class="muted">${escapeHtml(attrs.artistName || "")}</span>
    `;

        btn.addEventListener("click", async () => {
            try {
                await playSong(song);
            } catch (err) {
                setStatus(err.message);
            }
        });

        results.appendChild(btn);
    }
}

$("configure").addEventListener("click", async () => {
    try {
        await configureMusicKit();
    } catch (err) {
        setStatus(err.message);
    }
});

$("authorize").addEventListener("click", async () => {
    try {
        await authorize();
    } catch (err) {
        setStatus(err.message);
    }
});

$("search-form").addEventListener("submit", async e => {
    e.preventDefault();

    const query = $("query").value.trim();
    if (!query) return;

    try {
        setStatus("Searching...");
        const songs = await searchCatalog(query);
        renderSongs(songs);
        setStatus(`Found ${songs.length} song(s).`);
    } catch (err) {
        setStatus(err.message);
    }
});