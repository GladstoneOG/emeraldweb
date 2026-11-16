/* ============================================================================
 * Pokémon Emerald Web Multiplayer — Companion HUD Controller & Boot Orchestrator
 * ============================================================================ */
(function (global) {
  "use strict";

  const HOENN_BADGES = [
    { name: "Stone Badge (Rustboro)", icon: "icon-badge-stone" },
    { name: "Knuckle Badge (Dewford)", icon: "icon-badge-knuckle" },
    { name: "Dynamo Badge (Mauville)", icon: "icon-badge-dynamo" },
    { name: "Heat Badge (Lavaridge)", icon: "icon-badge-heat" },
    { name: "Balance Badge (Petalburg)", icon: "icon-badge-balance" },
    { name: "Feather Badge (Fortree)", icon: "icon-badge-feather" },
    { name: "Mind Badge (Mossdeep)", icon: "icon-badge-mind" },
    { name: "Rain Badge (Sootopolis)", icon: "icon-badge-rain" },
  ];

  const BALL_ICONS = {
    pokeball: "icon-pokeball",
    greatball: "icon-greatball",
    ultraball: "icon-ultraball",
    masterball: "icon-masterball",
  };

  const VALID_STATUS_TAGS = new Set(["BRN", "PAR", "PSN", "SLP", "FRZ", "FNT"]);

  const NUZLOCKE_LABELS = {
    standard_soullink: "Standard Soul-Link",
    hardcore: "Hardcore Nuzlocke",
    casual_coop: "Casual Co-op",
    none: "No Nuzlocke",
  };

  const TUTORIAL_GATE_TOOLTIP =
    "Finish starter tutorial (May battle, Pokédex, Poké Balls & Running Shoes) first";

  const TRADE_EVOLVABLE = new Set([
    "Kadabra",
    "Machoke",
    "Graveler",
    "Haunter",
    "Poliwhirl",
    "Slowpoke",
    "Onix",
    "Scyther",
    "Seadra",
    "Porygon",
    "Clamperl",
    "Dusclops",
    "Rhydon",
    "Electabuzz",
    "Magmar",
  ]);

  let currentSidebarTab = "parties";
  let audioMuted = localStorage.getItem("pokemon_web_audio_muted") === "1";
  let audioVolume = (() => {
    const raw = localStorage.getItem("pokemon_web_audio_volume");
    const parsed = raw !== null ? Number(raw) : 80;
    return Number.isFinite(parsed) ? Math.max(0, Math.min(100, Math.round(parsed))) : 80;
  })();
  let lastNonZeroVolume = audioVolume > 0 ? audioVolume : 80;
  let pendingTradeItems = []; // local draft items in trade modal
  let previewAnimStop = false;
  let lastGamepadMask = 0;
  let gamepadPollStarted = false;
  let trackedKeyboardMask = 0;
  let pendingLateJoinCtx = null;
  const lastRenderedHtml = new WeakMap();

  const DOM_CODE_TO_GBA_BIT = {
    KeyZ: 1 << 0,
    KeyK: 1 << 0,
    KeyX: 1 << 1,
    KeyJ: 1 << 1,
    ShiftLeft: 1 << 2,
    ShiftRight: 1 << 2,
    Backspace: 1 << 2,
    Enter: 1 << 3,
    NumpadEnter: 1 << 3,
    ArrowRight: 1 << 4,
    KeyD: 1 << 4,
    ArrowLeft: 1 << 5,
    KeyA: 1 << 5,
    ArrowUp: 1 << 6,
    KeyW: 1 << 6,
    ArrowDown: 1 << 7,
    KeyS: 1 << 7,
    KeyE: 1 << 8,
    KeyO: 1 << 8,
    KeyQ: 1 << 9,
    KeyU: 1 << 9,
  };

  function $(id) {
    return document.getElementById(id);
  }

  function setHtmlIfChanged(el, html) {
    if (!el) return;
    if (lastRenderedHtml.get(el) === html) return;
    lastRenderedHtml.set(el, html);
    el.innerHTML = html;
  }

  function setTextIfChanged(el, text) {
    if (!el) return;
    const next = String(text ?? "");
    if (el.textContent === next) return;
    el.textContent = next;
  }

  function escapeHtml(val) {
    return String(val ?? "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#39;");
  }

  function svgIcon(iconId, extraClass) {
    return `<svg class="em-icon ${extraClass || ""}"><use href="#${escapeHtml(iconId)}"></use></svg>`;
  }

  function focusCanvas() {
    const c = $("canvas");
    if (c) c.focus();
  }

  function copyText(text) {
    try {
      const ta = document.createElement("textarea");
      ta.value = text;
      ta.style.position = "fixed";
      ta.style.left = "-9999px";
      document.body.appendChild(ta);
      ta.select();
      document.execCommand("copy");
      document.body.removeChild(ta);
    } catch (_) {}
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).catch(() => {});
    }
  }

  function downloadJson(obj, filename) {
    const blob = new Blob([JSON.stringify(obj, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
  }

  function showToast(kind, title, message) {
    const stack = $("toast-stack");
    if (!stack) return;
    const el = document.createElement("div");
    el.className = "em-panel em-toast";
    if (kind === "danger") el.style.borderLeft = "5px solid #d93848";
    else if (kind === "gold") el.style.borderLeft = "5px solid #c99416";
    else if (kind === "ok") el.style.borderLeft = "5px solid #2d9575";
    else el.style.borderLeft = "5px solid #3a86ff";

    el.innerHTML = `
      <div class="gba-font" style="font-size:11px;font-weight:700;color:#154c3e;">${escapeHtml(title)}</div>
      <div style="font-size:12px;margin-top:2px;">${escapeHtml(message)}</div>
    `;
    stack.appendChild(el);
    setTimeout(() => {
      el.style.opacity = "0";
      el.style.transition = "opacity 0.25s";
      setTimeout(() => el.remove(), 260);
    }, 3600);
  }

  // --- Modals & Visibility ---
  function clearRoomsJoinError() {
    const errBox = $("rooms-join-error");
    if (errBox) errBox.style.display = "none";
  }

  async function attemptJoinRoomPort(port) {
    const numPort = Number(port);
    if (!(numPort > 0)) return false;
    clearRoomsJoinError();
    const room = (PokemonNet.remoteRooms || []).find((r) => Number(r.game_port) === numPort);
    if (room && room.game_over) {
      showToast("danger", "Run Ended", "This Soul-Link Nuzlocke run has ended (Game Over).");
      return false;
    }
    const isReturning = room && room.run_id ? await PokemonNet.hasReturningSlotForRun(room.run_id) : false;
    const badgeCount = Number(room && room.badge_count) || 0;
    const deathCount = Number(room && room.death_count) || 0;
    const caughtRoutesCount = Number(room && room.caught_routes_count) || 0;
    if (!isReturning && (badgeCount > 0 || deathCount > 0 || caughtRoutesCount > 0)) {
      openLateJoinModal({
        source: "browser",
        port: numPort,
        roomName: (room && room.room_name) || `Room #${numPort}`,
        hostName: (room && room.host_name) || "Host",
        nuzlockePreset: room && room.nuzlocke_preset,
        badgeCount,
        deathCount,
        caughtRoutesCount,
        graveyard: [],
        deadRoutes: [],
      });
      return false;
    }
    PokemonNet.joinRoom(numPort, { confirmedProgressed: isReturning });
    closeModal("rooms-modal");
    return true;
  }

  function openModal(id) {
    for (const el of document.querySelectorAll(".em-modal-backdrop")) {
      el.classList.remove("open");
    }
    const target = $(id);
    if (target) target.classList.add("open");
    if (id === "rooms-modal") {
      syncHostRandomizerPresetToForm();
      renderRoomBrowserList();
      PokemonNet.refreshRooms();
      renderSavedRunsLists();
    } else if (id === "saves-modal") {
      renderSavesModal();
    } else if (id === "trade-modal") {
      renderTradeModal();
    }
  }

  function closeModal(id) {
    const target = $(id);
    if (target) target.classList.remove("open");
    if (id === "late-join-modal" && pendingLateJoinCtx) {
      const ctx = pendingLateJoinCtx;
      pendingLateJoinCtx = null;
      if (ctx.source === "welcome") {
        if (typeof PokemonNet.cancelLateJoin === "function") {
          PokemonNet.cancelLateJoin();
        } else {
          PokemonNet.disconnect();
        }
      } else if (ctx.source === "browser") {
        openModal("rooms-modal");
        return;
      }
    }
    if (id === "slot-claim-modal" && PokemonNet._pendingSlotClaim) {
      PokemonNet.cancelSlotClaim();
    }
    focusCanvas();
  }

  function toggleHud(hide) {
    if (hide === undefined) document.body.classList.toggle("hud-hidden");
    else if (hide) document.body.classList.add("hud-hidden");
    else document.body.classList.remove("hud-hidden");
    focusCanvas();
  }

  function toggleSidebar() {
    document.body.classList.toggle("sidebar-collapsed");
    focusCanvas();
  }

  function syncFullscreenEscapeLock() {
    if (document.fullscreenElement) {
      if (navigator.keyboard && typeof navigator.keyboard.lock === "function") {
        navigator.keyboard.lock(["Escape"]).catch(() => {});
      }
    } else if (navigator.keyboard && typeof navigator.keyboard.unlock === "function") {
      try {
        navigator.keyboard.unlock();
      } catch (_) {}
    }
  }

  function toggleFullscreen() {
    if (!document.fullscreenElement) {
      document.documentElement
        .requestFullscreen()
        .then(syncFullscreenEscapeLock)
        .catch(() => {});
    } else {
      document.exitFullscreen().catch(() => {});
    }
    focusCanvas();
  }

  function syncAudioContextMuteState() {
    if (global._pokeAudioGain && global._pokeAudioGain.gain) {
      global._pokeAudioGain.gain.value = audioMuted ? 0 : audioVolume / 100;
    }
    const contexts = [];
    if (global._pokeAudioCtx) contexts.push(global._pokeAudioCtx);
    if (global.Module && global.Module.SDL2 && global.Module.SDL2.audioContext) {
      contexts.push(global.Module.SDL2.audioContext);
    }
    for (const ctx of contexts) {
      try {
        if (audioMuted) {
          global._pokeNextAudioTime = 0;
          if (ctx.state !== "suspended" && ctx.state !== "closed" && typeof ctx.suspend === "function") {
            Promise.resolve(ctx.suspend()).catch(() => {});
          }
        } else if (ctx.state !== "running" && ctx.state !== "closed" && typeof ctx.resume === "function") {
          Promise.resolve(ctx.resume()).catch(() => {});
        }
      } catch (_) {}
    }
  }

  function applyAudioMuteState() {
    const effectiveMuted = audioMuted || audioVolume <= 0;
    global.POKEMON_AUDIO_MUTED = effectiveMuted;
    global.POKEMON_AUDIO_VOLUME = audioVolume / 100;
    localStorage.setItem(STORAGE_KEYS_AUDIO(), effectiveMuted ? "1" : "0");
    localStorage.setItem("pokemon_web_audio_volume", String(audioVolume));
    const iconEl = $("audio-mute-icon");
    if (iconEl) {
      setHtmlIfChanged(iconEl, `<use href="#${effectiveMuted ? "icon-audio-off" : "icon-audio-on"}"></use>`);
    }
    const btn = $("audio-mute-btn");
    if (btn) {
      btn.title = effectiveMuted ? "Audio Muted (Click to Unmute)" : `Mute Audio (${audioVolume}%)`;
    }
    const sliderEl = $("audio-volume-slider");
    if (sliderEl) {
      sliderEl.value = String(effectiveMuted ? 0 : audioVolume);
    }
    const pctEl = $("audio-volume-pct");
    if (pctEl) {
      pctEl.textContent = `${effectiveMuted ? 0 : audioVolume}%`;
    }
    syncAudioContextMuteState();
  }

  function STORAGE_KEYS_AUDIO() {
    return "pokemon_web_audio_muted";
  }

  // --- Gate & Status Helpers ---
  function formatBusyReasonText(reason, who, mapName) {
    const cleanWho = who || "Teammate";
    switch (reason) {
      case "different_map":
        return mapName
          ? `${cleanWho} is on a different map (${mapName}) — continuing battle solo.`
          : `${cleanWho} is on a different map — continuing battle solo.`;
      case "in_tutorial":
        return `${cleanWho} is still finishing the starter tutorial — continuing battle solo.`;
      case "in_battle":
        return `${cleanWho} is already in another battle — continuing battle solo.`;
      case "not_ready":
        return `${cleanWho} is still loading or syncing into Hoenn — continuing battle solo.`;
      case "busy":
        return `${cleanWho} is in a menu, dialogue, or cutscene — continuing battle solo.`;
      default:
        return `${cleanWho} is unavailable right now — continuing battle solo.`;
    }
  }

  function getUiFeatureGateReason(state, targetSlotIndex) {
    if (state && state.runConfig && state.runConfig.gameOver) {
      return "Run is Game Over";
    }
    if (typeof PokemonNet.getFeatureGateReason === "function") {
      const bridgeReason = PokemonNet.getFeatureGateReason(targetSlotIndex);
      if (bridgeReason) return bridgeReason;
    }
    const myIdx = (state && state.connection && state.connection.mySlotIndex) || 0;
    const me = state && state.players ? state.players[myIdx] : null;
    if (!me || !me.tutorialComplete) {
      return TUTORIAL_GATE_TOOLTIP;
    }
    if (targetSlotIndex !== undefined && targetSlotIndex !== null) {
      const peer = state && state.players ? state.players[Number(targetSlotIndex)] : null;
      if (peer && !peer.tutorialComplete) {
        return `${peer.name || "Peer"} must finish starter tutorial (May battle, Pokédex, Poké Balls & Running Shoes) first`;
      }
    }
    return null;
  }

  function getRouteLockStatusInfo(lock) {
    if (lock && lock.failed) return { key: "failed", label: "FAILED" };
    if (lock && lock.dead) return { key: "dead", label: "DEAD" };
    return { key: "alive", label: "ALIVE" };
  }

  function resolveRouteMemberLabel(state, slotIdx, monUid, linkId) {
    const numSlot = Number(slotIdx);
    const player = state && state.players ? state.players[numSlot] : null;
    const playerName = (player && player.name) || `P${numSlot + 1}`;
    const party = player && Array.isArray(player.party) ? player.party : [];
    let mon = party.find((m) => m && ((monUid && m.uid === monUid) || (linkId && m.linkId === linkId)));
    if (!mon && state && state.soulLink && Array.isArray(state.soulLink.graveyard)) {
      mon = state.soulLink.graveyard.find(
        (g) =>
          g &&
          Number(g.slotIndex) === numSlot &&
          ((monUid && g.uid === monUid) || (linkId && g.linkId === linkId) || (monUid && g.species === monUid))
      );
    }
    const monText = mon
      ? `${mon.nickname || mon.species} (${mon.species})`
      : monUid && String(monUid).startsWith("mon-")
        ? "Caught (PC / Linked)"
        : monUid || "Caught";
    return `P${numSlot + 1} ${playerName}: ${monText}`;
  }

  // --- Renderers ---
  function renderTopbar(state) {
    const badge = $("net-status-badge");
    const tutorialBadge = $("local-tutorial-badge");
    const gameOverBadge = $("run-gameover-badge");
    const tradeBtn = $("open-trade-btn");
    const inviteBtn = $("copy-invite-btn");
    const seedPill = $("run-seed-pill");
    const disconnectBtn = $("disconnect-room-btn");
    const speedGroup = $("speed-control-group");
    const speedReadonly = $("speed-readonly-pill");
    const connectedCount = state.players.filter((p) => p.connected).length;
    const myIdx = (state.connection && state.connection.mySlotIndex) || 0;
    const me = state.players[myIdx];
    const gameOver = Boolean(state.runConfig && state.runConfig.gameOver);
    const localInTutorial = !me || !me.tutorialComplete;

    if (tutorialBadge) {
      tutorialBadge.style.display = localInTutorial ? "inline-flex" : "none";
      tutorialBadge.title = TUTORIAL_GATE_TOOLTIP;
    }
    if (gameOverBadge) {
      gameOverBadge.style.display = gameOver ? "inline-flex" : "none";
    }
    if (tradeBtn) {
      const localGate = getUiFeatureGateReason(state);
      tradeBtn.disabled = Boolean(localGate);
      tradeBtn.title = localGate || "Anywhere Quick-Trade";
    }

    if (badge) {
      const roomNum = Number(state.connection.roomPort) || 0;
      if (state.connection.status === "hosting") {
        if (badge.className !== "em-badge host") badge.className = "em-badge host";
        setHtmlIfChanged(badge, `${svgIcon("icon-host-crown")} HOST #${roomNum} (${connectedCount}/4)`);
        if (inviteBtn) inviteBtn.style.display = "inline-flex";
        if (disconnectBtn) disconnectBtn.style.display = "inline-flex";
      } else if (state.connection.status === "connected") {
        if (badge.className !== "em-badge ok") badge.className = "em-badge ok";
        setHtmlIfChanged(badge, `${svgIcon("icon-pokeball")} ROOM #${roomNum} (${connectedCount}/4)`);
        if (inviteBtn) inviteBtn.style.display = "inline-flex";
        if (disconnectBtn) disconnectBtn.style.display = "inline-flex";
      } else if (state.connection.status === "connecting") {
        if (badge.className !== "em-badge") badge.className = "em-badge";
        setHtmlIfChanged(badge, `${svgIcon("icon-pokeball")} Connecting...`);
        if (inviteBtn) inviteBtn.style.display = "none";
        if (disconnectBtn) disconnectBtn.style.display = "inline-flex";
      } else {
        if (badge.className !== "em-badge") badge.className = "em-badge";
        setHtmlIfChanged(badge, `${svgIcon("icon-pokeball")} Offline / Solo`);
        if (inviteBtn) inviteBtn.style.display = "none";
        if (disconnectBtn) disconnectBtn.style.display = "none";
      }
    }

    setTextIfChanged(seedPill, `SEED: ${state.runConfig.seed}`);

    // Host-Only Speed Control Rendering
    const spd = Number(state.runConfig.speedMultiplier) || 1;
    const isClient = state.connection.status === "connected";
    if (speedGroup) {
      speedGroup.classList.toggle("client-locked", isClient);
      for (const btn of speedGroup.querySelectorAll(".speed-btn")) {
        const btnSpd = Number(btn.dataset.speed);
        btn.classList.toggle("active", btnSpd === spd);
        btn.disabled = isClient;
      }
    }
    setHtmlIfChanged(speedReadonly, `${svgIcon("icon-host-crown")} HOST: ${spd}x`);

    const previewControls = $("preview-sim-controls");
    const previewPill = $("wasm-preview-pill");
    if (state.wasmReady) {
      if (previewControls) previewControls.style.display = "none";
      if (previewPill) previewPill.classList.add("hidden");
    }

    setTextIfChanged($("room-count-num"), String((PokemonNet.remoteRooms || []).length));
    renderAuthUi(state);
  }

  function renderAuthUi(state) {
    const auth = (state && state.auth) || { status: "signed_out", username: "", userId: "" };
    const signedIn = auth.status === "signed_in" && Boolean(auth.username);
    const userBadge = $("account-user-badge");
    const openAuthBtn = $("open-auth-btn");
    const logoutBtn = $("account-logout-btn");
    const savesAuthText = $("saves-auth-status-text");
    const savesOpenAuthBtn = $("saves-open-auth-btn");

    if (userBadge) {
      userBadge.style.display = signedIn ? "inline-flex" : "none";
      if (signedIn) setTextIfChanged(userBadge, `@${auth.username}`);
    }
    if (openAuthBtn) {
      openAuthBtn.style.display = signedIn ? "none" : "inline-flex";
    }
    if (logoutBtn) {
      logoutBtn.style.display = signedIn ? "inline-flex" : "none";
    }
    if (savesAuthText) {
      savesAuthText.textContent = signedIn
        ? `Signed in as @${auth.username} — Hosted & Joined runs sync to SQLite cloud storage.`
        : "Sign in to sync Hosted & Joined multiplayer runs to your account across browsers & cache clears.";
    }
    if (savesOpenAuthBtn) {
      savesOpenAuthBtn.style.display = signedIn ? "none" : "inline-flex";
    }
  }

  function renderCoopBar(state) {
    const badgeStrip = $("hoenn-badges-strip");
    if (badgeStrip) {
      setHtmlIfChanged(
        badgeStrip,
        HOENN_BADGES.map((b, idx) => {
          const earned = Boolean(state.world.badges[idx]);
          return `
          <button
            type="button"
            class="gym-badge-slot ${earned ? "earned" : ""}"
            style="background:transparent;border:none;padding:0;cursor:${PokemonNet.isHost() ? "pointer" : "default"};"
            title="${escapeHtml(b.name)} (${earned ? "Earned" : "Locked"})"
            data-toggle-badge="${idx}"
          >
            ${svgIcon(b.icon, "em-icon-lg")}
          </button>
        `;
        }).join("")
      );
    }

    const peerStrip = $("peer-actions-strip");
    if (peerStrip) {
      const myIdx = state.connection.mySlotIndex;
      const myPlayer = state.players[myIdx];
      const activePeers = state.players.filter((p) => p.connected);
      const gameOver = Boolean(state.runConfig && state.runConfig.gameOver);

      setHtmlIfChanged(
        peerStrip,
        activePeers
          .map((p) => {
            const slotNum = Number(p.slotIndex) || 0;
            const isMe = slotNum === myIdx;
            const inTutorial = !p.tutorialComplete;
            const isPairedWithMe = Boolean(myPlayer && myPlayer.partyId && p.partyId === myPlayer.partyId && !isMe);
            const roleIcon = p.isHost
              ? svgIcon("icon-host-crown")
              : p.partyId
                ? svgIcon("icon-coop-shield")
                : svgIcon("icon-pokeball");

            const tutorialPill = inTutorial
              ? `<span class="em-badge tutorial peer-tutorial-pill" title="${escapeHtml(TUTORIAL_GATE_TOOLTIP)}">TUTORIAL</span>`
              : "";

            const peerGate = getUiFeatureGateReason(state, slotNum);
            const peerDisabledAttr = peerGate ? "disabled" : "";
            const leaveDisabledAttr = gameOver ? "disabled" : "";
            const pairTitle = isPairedWithMe
              ? gameOver
                ? "Run is Game Over"
                : "Leave 2v2 Party"
              : peerGate || "Pair for Co-op 2v2";

            const actionButtons = isMe
              ? myPlayer.partyId
                ? `<button class="em-btn sm" type="button" data-action="leave-party" ${leaveDisabledAttr} title="${escapeHtml(gameOver ? "Run is Game Over" : "Leave Co-op Party")}">${svgIcon("icon-coop-shield")} ${escapeHtml(myPlayer.partyId)} ✕</button>`
                : ""
              : `
              <button class="em-btn sm ${isPairedWithMe ? "gold" : ""}" type="button" data-action="${isPairedWithMe ? "leave-party" : "pair-party"}" data-slot="${slotNum}" ${isPairedWithMe ? leaveDisabledAttr : peerDisabledAttr} title="${escapeHtml(pairTitle)}">
                ${svgIcon("icon-coop-shield")} ${isPairedWithMe ? "Paired" : "Pair"}
              </button>
              <button class="em-btn sm" type="button" data-action="teleport" data-slot="${slotNum}" ${peerDisabledAttr} title="${escapeHtml(peerGate || `Teleport to ${p.name} (${p.mapName})`)}">
                ${svgIcon("icon-warp-pad")} Warp
              </button>
              <button class="em-btn sm" type="button" data-action="pvp" data-slot="${slotNum}" ${peerDisabledAttr} title="${escapeHtml(peerGate || "Friendly PvP (Nuzlocke Safe)")}">
                ${svgIcon("icon-pvp-swords")} PvP
              </button>
              <button class="em-btn sm" type="button" data-action="trade" data-slot="${slotNum}" ${peerDisabledAttr} title="${escapeHtml(peerGate || `Quick-Trade with ${p.name}`)}">
                ${svgIcon("icon-trade-arrows")}
              </button>
            `;

            return `
            <div class="peer-chip ${isMe ? "self" : ""} ${p.partyId ? "paired" : ""} ${inTutorial ? "tutorial" : ""} ${gameOver ? "gameover" : ""}">
              ${roleIcon}
              <strong class="gba-font" style="font-size:11px;">P${slotNum + 1} ${escapeHtml(p.name)}</strong>
              ${tutorialPill}
              <span class="peer-map-tag">${escapeHtml(p.mapName)}</span>
              ${actionButtons}
            </div>
          `;
          })
          .join("")
      );
    }
  }

  function renderSoulLinkSidebar(state) {
    setTextIfChanged(
      $("soullink-mode-badge"),
      state.runConfig.soulLinkMode === "group" ? "FULL GROUP" : "2P PAIRS"
    );
    const gameOverBanner = $("sidebar-gameover-banner");
    if (gameOverBanner) {
      gameOverBanner.style.display = state.runConfig && state.runConfig.gameOver ? "flex" : "none";
    }
    const routeLocksObj = (state.soulLink && state.soulLink.routeLocks) || {};
    const routeLocks = Object.values(routeLocksObj);
    const deadRoutesCount = routeLocks.filter((lk) => lk && (lk.dead || lk.failed)).length;
    const aliveRoutesCount = routeLocks.length - deadRoutesCount;
    const routesTabBtn = $("tab-btn-routes");
    if (routesTabBtn) {
      routesTabBtn.title = `Routes: ${aliveRoutesCount} Alive, ${deadRoutesCount} Dead/Failed (${routeLocks.length} Locked)`;
    }
    const graveTabBtn = $("tab-btn-graveyard");
    if (graveTabBtn) {
      graveTabBtn.title = `Graveyard (${(state.soulLink.graveyard || []).length})`;
    }

    for (const tabBtn of document.querySelectorAll("[data-sidebar-tab]")) {
      tabBtn.classList.toggle("active", tabBtn.dataset.sidebarTab === currentSidebarTab);
    }

    const content = $("sidebar-content");
    if (!content) return;

    if (currentSidebarTab === "parties") {
      const visiblePlayers = state.players.filter((p) => p.connected || (p.party && p.party.length > 0));
      const nextHtml = visiblePlayers
        .map((p) => {
          const slotsHtml = [];
          for (let i = 0; i < 6; i++) {
            const mon = p.party[i];
            if (!mon) {
              slotsHtml.push(`<div class="mon-slot empty">EMPTY</div>`);
              continue;
            }
            const pct = mon.maxHp > 0 ? Math.max(0, Math.min(100, Math.round((Number(mon.hp) / Number(mon.maxHp)) * 100))) : 0;
            const hpClass = pct <= 20 ? "crit" : pct <= 50 ? "warn" : "";
            const ballIcon = Object.prototype.hasOwnProperty.call(BALL_ICONS, mon.ball)
              ? BALL_ICONS[mon.ball]
              : "icon-pokeball";
            const highlighted = Boolean(state.highlightedLinkId && mon.linkId === state.highlightedLinkId);
            const cleanStatus = VALID_STATUS_TAGS.has(mon.status) ? mon.status : "";
            const statusHtml = cleanStatus
              ? `<span class="status-tag ${cleanStatus}">${cleanStatus}</span>`
              : "";

            slotsHtml.push(`
              <div
                class="mon-slot ${mon.isDead ? "fainted" : ""} ${highlighted ? "link-highlighted" : ""}"
                data-link-id="${escapeHtml(mon.linkId || "")}"
                title="${escapeHtml(mon.nickname)} (${escapeHtml(mon.species)}) • ${escapeHtml(mon.originMap)} • Click to highlight Soul-Link"
              >
                <div class="mon-top-row">
                  <div style="display:flex;align-items:center;gap:4px;min-width:0;">
                    ${svgIcon(ballIcon)}
                    <span class="mon-name">${escapeHtml(mon.nickname || mon.species)}</span>
                  </div>
                  <span class="mon-lv">Lv.${Number(mon.level) || 0}</span>
                </div>
                <div class="em-hp-track">
                  <div class="em-hp-fill ${hpClass}" style="width:${pct}%"></div>
                </div>
                <div class="mon-meta-row">
                  <span class="link-pill">
                    ${svgIcon(mon.isDead ? "icon-link-broken" : "icon-link-chain", "")}
                    ${escapeHtml(mon.linkId || "SOLO")}
                  </span>
                  <span>${statusHtml || `${Number(mon.hp) || 0}/${Number(mon.maxHp) || 0}`}</span>
                </div>
              </div>
            `);
          }

          const slotNum = Number(p.slotIndex) || 0;
          const headBadge = p.isHost
            ? svgIcon("icon-host-crown")
            : p.partyId
              ? svgIcon("icon-coop-shield")
              : svgIcon("icon-pokeball");

          return `
            <section class="matrix-player-card">
              <div class="matrix-player-head ${p.connected ? "" : "offline"}">
                <div style="display:flex;align-items:center;gap:5px;min-width:0;">
                  ${headBadge}
                  <span style="font-weight:700;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">
                    P${slotNum + 1} ${escapeHtml(p.name)}
                  </span>
                  ${p.partyId ? `<span class="em-badge host" style="padding:1px 4px;font-size:9px;">${escapeHtml(p.partyId)}</span>` : ""}
                  ${!p.tutorialComplete ? `<span class="em-badge tutorial sm" title="${escapeHtml(TUTORIAL_GATE_TOOLTIP)}">TUTORIAL</span>` : ""}
                </div>
                <span style="font-size:10px;opacity:0.92;">¥${Number(p.money || 0).toLocaleString()}</span>
              </div>
              <div class="matrix-party-grid">${slotsHtml.join("")}</div>
            </section>
          `;
        })
        .join("");
      setHtmlIfChanged(content, nextHtml);
    } else if (currentSidebarTab === "routes") {
      if (routeLocks.length === 0) {
        setHtmlIfChanged(
          content,
          `
          <div class="em-panel" style="padding:16px;text-align:center;">
            ${svgIcon("icon-link-chain", "em-icon-xl")}
            <div class="gba-font" style="font-size:12px;font-weight:700;margin-top:6px;">NO ROUTES LOCKED YET</div>
            <div style="font-size:11px;color:#55626d;margin-top:4px;">First wild encounter on each Hoenn route locks that route for your Soul-Link group.</div>
          </div>
        `
        );
      } else {
        const sortedLocks = routeLocks.slice().sort((a, b) => {
          const aDead = a && (a.dead || a.failed) ? 1 : 0;
          const bDead = b && (b.dead || b.failed) ? 1 : 0;
          if (aDead !== bDead) return bDead - aDead;
          return String((a && (a.routeName || a.routeId)) || "").localeCompare(
            String((b && (b.routeName || b.routeId)) || "")
          );
        });
        const summaryHtml = `
          <div class="routes-summary-bar">
            <span>LOCKED ROUTES: ${routeLocks.length}</span>
            <span>${aliveRoutesCount} ALIVE • ${deadRoutesCount} DEAD/FAILED</span>
          </div>
        `;
        const cardsHtml = sortedLocks
          .map((lock) => {
            if (!lock) return "";
            const st = getRouteLockStatusInfo(lock);
            const highlighted = Boolean(state.highlightedLinkId && lock.linkId === state.highlightedLinkId);
            const memberEntries = Object.entries(lock.members || {});
            const membersHtml =
              memberEntries.length > 0
                ? memberEntries
                    .map(
                      ([slotIdx, monUid]) =>
                        `<span class="route-member-chip">${escapeHtml(
                          resolveRouteMemberLabel(state, slotIdx, monUid, lock.linkId)
                        )}</span>`
                    )
                    .join("")
                : `<span style="font-style:italic;">No surviving linked Pokémon recorded</span>`;
            return `
              <div
                class="route-lock-card ${st.key} ${highlighted ? "link-highlighted" : ""}"
                data-link-id="${escapeHtml(lock.linkId || "")}"
                title="Group: ${escapeHtml(lock.groupKey || "ALL")} • Click to highlight ${escapeHtml(lock.linkId || "route")}"
              >
                <div class="route-lock-head">
                  <span class="route-lock-name">${escapeHtml(lock.routeName || lock.routeId || "Unknown Route")}</span>
                  <div style="display:flex;align-items:center;gap:4px;">
                    ${lock.linkId ? `<span class="link-pill">${escapeHtml(lock.linkId)}</span>` : ""}
                    <span class="route-lock-status ${st.key}">${st.label}</span>
                  </div>
                </div>
                <div class="route-lock-members">
                  <span class="route-member-chip">${escapeHtml(lock.groupKey || "ALL")}</span>
                  ${membersHtml}
                </div>
              </div>
            `;
          })
          .join("");
        setHtmlIfChanged(content, summaryHtml + cardsHtml);
      }
    } else if (currentSidebarTab === "graveyard") {
      const graves = state.soulLink.graveyard || [];
      if (graves.length === 0) {
        setHtmlIfChanged(
          content,
          `
          <div class="em-panel" style="padding:16px;text-align:center;">
            ${svgIcon("icon-tombstone", "em-icon-xl")}
            <div class="gba-font" style="font-size:12px;font-weight:700;margin-top:6px;">GRAVEYARD IS EMPTY</div>
            <div style="font-size:11px;color:#55626d;margin-top:4px;">Keep your Soul-Link chains unbroken!</div>
          </div>
        `
        );
      } else {
        setHtmlIfChanged(
          content,
          graves
            .map(
              (g) => `
          <div class="grave-card">
            ${svgIcon("icon-tombstone", "em-icon-lg")}
            <div class="grave-info">
              <div class="grave-title">${escapeHtml(g.nickname)} (${escapeHtml(g.species)}) • Lv.${Number(g.level)}</div>
              <div class="grave-sub">
                Trainer: <strong>${escapeHtml(g.trainerName)}</strong> • ${escapeHtml(g.linkId)} (${escapeHtml(g.originMap)})
              </div>
              <div class="grave-sub" style="color:#a32531;">${escapeHtml(g.cause)} • ${escapeHtml(g.diedAt)}</div>
            </div>
            ${svgIcon("icon-link-broken")}
          </div>
        `
            )
            .join("")
        );
      }
    } else {
      setHtmlIfChanged(
        content,
        (state.eventLog || [])
          .map(
            (ev) => `
        <div class="event-log-item ${ev.kind === "death" ? "death" : ev.kind === "gold" ? "gold" : ""}">
          <span class="gba-font" style="font-size:9px;color:#55626d;">[${escapeHtml(ev.time)}]</span>
          ${escapeHtml(ev.text)}
        </div>
      `
          )
          .join("")
      );
    }
  }

  function renderBattleInviteToast(state) {
    const toast = $("battle-join-toast");
    if (!toast) return;
    const inv = state.activeInvites[0];
    if (!inv) {
      toast.classList.remove("open");
      return;
    }
    toast.classList.add("open");
    $("battle-toast-title").textContent =
      inv.mode === "pvp" ? "FRIENDLY PvP CHALLENGE" : "CROSS-MAP CO-OP 2v2 INVITE";
    $("battle-toast-desc").textContent =
      `${inv.fromName} @ ${inv.mapName}: ${inv.enemyDesc}. Join now?`;
  }

  function renderCutsceneBanner(state) {
    const banner = $("cutscene-sync-banner");
    if (!banner) return;
    const cs = state.activeCutscene;
    if (!cs) {
      banner.classList.remove("open");
      return;
    }
    banner.classList.add("open");
    $("cutscene-title-text").textContent = `${cs.title} (Triggered by ${cs.triggeredBy})`;
    const finishBtn = $("cutscene-finish-btn");
    if (finishBtn) {
      finishBtn.textContent = PokemonNet.isHost() ? "Complete & Return All" : "Mark Ready";
    }

    const peersRow = $("cutscene-peers-row");
    if (peersRow) {
      setHtmlIfChanged(
        peersRow,
        state.players
          .filter((p) => p.connected)
          .map((p) => {
            const slotNum = Number(p.slotIndex) || 0;
            const st = (cs.peerStates && cs.peerStates[slotNum]) || "watching";
            const cls = st === "in_battle_deferred" ? "warn" : st === "done" ? "ok" : "host";
            const label = st === "in_battle_deferred" ? "Finishing Battle" : st === "done" ? "Ready" : "Watching";
            return `<span class="em-badge ${cls}">P${slotNum + 1} ${escapeHtml(p.name)}: ${label}</span>`;
          })
          .join("")
      );
    }
  }

  function renderRoomBrowserList() {
    const relayLabel = $("relay-status-label");
    if (relayLabel) {
      const base = global.POKEMON_RELAY_URL || "";
      if (base) {
        setTextIfChanged(relayLabel, `Relay: ${base.replace(/^https?:\/\//, "")}`);
      } else if (PokemonNet.lanInfo && Array.isArray(PokemonNet.lanInfo.lanIps) && PokemonNet.lanInfo.lanIps.length > 0) {
        setTextIfChanged(relayLabel, `LAN: http://${PokemonNet.lanInfo.lanIps[0]}:${PokemonNet.lanInfo.port || 8080}`);
      } else {
        setTextIfChanged(relayLabel, `Relay: ${global.location ? global.location.host : "local"}`);
      }
    }

    const listEl = $("rooms-list-container");
    if (!listEl) return;
    const rooms = PokemonNet.remoteRooms || [];
    if (rooms.length === 0) {
      setHtmlIfChanged(
        listEl,
        `<div style="padding:14px;text-align:center;color:#55626d;">No active rooms found on this relay. Click "Host New Run" to start!</div>`
      );
      return;
    }
    setHtmlIfChanged(
      listEl,
      rooms
        .map((r) => {
          const gameOver = Boolean(r.game_over);
          const memberCount = Number(r.member_count) || 0;
          const memberLimit = Number(r.member_limit || 4);
          const isFull = memberCount >= memberLimit || (r.joinable === false && !gameOver);
          const badgeCount = Number(r.badge_count) || 0;
          const deathCount = Number(r.death_count) || 0;
          const caughtCount = Number(r.caught_routes_count) || 0;
          const nuzPresetKey = r.nuzlocke_preset || "standard_soullink";
          const nuzPresetLabel = NUZLOCKE_LABELS[nuzPresetKey] || nuzPresetKey;
          const joinDisabled = gameOver || isFull;
          const joinTitle = gameOver
            ? "This Soul-Link Nuzlocke run has ended (Game Over)."
            : isFull
              ? `Room is full (${memberCount}/${memberLimit})`
              : `Join ${r.room_name || "Room #" + r.game_port}`;

          return `
      <div class="em-list-row">
        <div style="min-width:0;">
          <div class="gba-font" style="font-weight:700;color:#154c3e;">${escapeHtml(r.room_name || "Room #" + r.game_port)}</div>
          <div style="font-size:11px;color:#55626d;">
            Host: <strong>${escapeHtml(r.host_name)}</strong> • #${Number(r.game_port)} •
            ${memberCount}/${memberLimit} Players •
            Seed: ${escapeHtml(r.seed || "—")} •
            ${escapeHtml(r.randomizer_preset || "balanced")} / ${escapeHtml(r.soul_link_mode || "pairs")} •
            Speed ${Number(r.speed_multiplier || 1)}x
          </div>
          <div class="room-meta-pills">
            <span class="em-badge sm">${escapeHtml(nuzPresetLabel)}</span>
            <span class="em-badge ${badgeCount > 0 ? "host" : ""} sm">${badgeCount}/8 Badges</span>
            <span class="em-badge ${deathCount > 0 ? "warn" : ""} sm">${deathCount} Deaths</span>
            ${caughtCount > 0 ? `<span class="em-badge sm">${caughtCount} Routes</span>` : ""}
            ${gameOver ? `<span class="em-badge gameover sm">GAME OVER</span>` : ""}
          </div>
        </div>
        <button class="em-btn ${gameOver ? "danger" : "primary"} sm" type="button" data-join-room="${Number(r.game_port)}" ${joinDisabled ? "disabled" : ""} title="${escapeHtml(joinTitle)}">Join</button>
      </div>
    `;
        })
        .join("")
    );
  }

  function summarizePartyShort(party) {
    if (!Array.isArray(party) || party.length === 0) return "No Pokémon yet";
    return party
      .slice(0, 3)
      .map((m) => `${m.nickname || m.species} Lv.${Number(m.level) || 1}`)
      .join(", ") + (party.length > 3 ? ` +${party.length - 3}` : "");
  }

  function buildSlotChipsHtml(slots, myUserId) {
    if (!Array.isArray(slots) || slots.length === 0) return "";
    const occupied = slots.filter(
      (s) => s && (s.userId || s.username || s.token || (Array.isArray(s.party) && s.party.length > 0) || s.tutorialComplete)
    );
    if (!occupied.length) return "";
    return `<div class="slot-chip-list">${occupied
      .map((s) => {
        const idx = Number(s.slotIdx ?? s.slotIndex ?? 0);
        const label = s.characterName || s.name || s.username || `Slot ${idx + 1}`;
        const partyCnt = Array.isArray(s.party) ? s.party.length : 0;
        const isMine = myUserId && s.userId && s.userId === myUserId;
        return `<span class="slot-chip ${isMine ? "mine" : ""}">P${idx + 1} ${escapeHtml(label)} (${partyCnt} mon)</span>`;
      })
      .join("")}</div>`;
  }

  async function renderSavedRunsLists() {
    const localRuns = await PokemonNet.listSavedRuns();
    const cloud = PokemonNet.cloudRuns || { hostedRuns: [], joinedRuns: [] };
    const auth = PokemonNet.state.auth || {};
    const myUserId = auth.status === "signed_in" ? auth.userId : "";
    const myToken = PokemonNet.state.connection && PokemonNet.state.connection.myToken;
    const remoteRooms = PokemonNet.remoteRooms || [];
    const resumeEl = $("resume-runs-list");
    const savesEl = $("saves-list-container");

    const cloudHostedMap = new Map();
    for (const r of cloud.hostedRuns || []) {
      if (r && r.runId) cloudHostedMap.set(r.runId, r);
    }
    const cloudJoinedMap = new Map();
    for (const r of cloud.joinedRuns || []) {
      if (r && r.runId) cloudJoinedMap.set(r.runId, r);
    }
    const localMap = new Map();
    for (const r of localRuns || []) {
      if (r && r.runId) localMap.set(r.runId, r);
    }

    const hostedEntries = [];
    const joinedEntries = [];
    const soloEntries = [];
    const seenIds = new Set();

    for (const r of cloud.hostedRuns || []) {
      if (!r || !r.runId) continue;
      seenIds.add(r.runId);
      hostedEntries.push({ kind: "hosted", runId: r.runId, cloud: r, local: localMap.get(r.runId) || null });
    }
    for (const r of cloud.joinedRuns || []) {
      if (!r || !r.runId || seenIds.has(r.runId)) continue;
      seenIds.add(r.runId);
      joinedEntries.push({ kind: "joined", runId: r.runId, cloud: r, local: localMap.get(r.runId) || null });
    }
    for (const r of localRuns || []) {
      if (!r || !r.runId || seenIds.has(r.runId)) continue;
      seenIds.add(r.runId);
      const slots = Array.isArray(r.playerSlots) ? r.playerSlots : [];
      const peerInPeerSlot = slots.some(
        (p, idx) =>
          idx > 0 &&
          p &&
          ((myUserId && p.userId === myUserId) || (myToken && p.token === myToken))
      );
      const hostSlotMatch =
        slots[0] && ((myUserId && slots[0].userId === myUserId) || (myToken && slots[0].token === myToken));
      const hasOtherPeers = slots.slice(1).some(
        (p) => p && (p.userId || p.token || (Array.isArray(p.party) && p.party.length > 0) || p.tutorialComplete)
      );
      if (peerInPeerSlot && !hostSlotMatch) {
        joinedEntries.push({ kind: "joined", runId: r.runId, cloud: null, local: r });
      } else if (hasOtherPeers) {
        hostedEntries.push({ kind: "hosted", runId: r.runId, cloud: null, local: r });
      } else {
        soloEntries.push({ kind: "solo", runId: r.runId, cloud: null, local: r });
      }
    }

    const renderGroupRows = (entries, emptyText, includeManageBtns, groupType) => {
      if (!entries.length) {
        return `<div class="save-group-empty">${escapeHtml(emptyText)}</div>`;
      }
      return entries
        .map(({ cloud: cr, local: lr, runId }) => {
          const worldObj = (cr && cr.worldState && cr.worldState.world) || (lr && lr.world) || {};
          const badgeCount = Array.isArray(worldObj.badges) ? worldObj.badges.filter(Boolean).length : 0;
          const seed = (cr && cr.seed) || (lr && lr.runConfig && lr.runConfig.seed) || "—";
          const name = (cr && cr.title) || (lr && lr.runConfig && lr.runConfig.roomName) || runId;
          const updatedTs = (cr && cr.updatedAt) || (lr && lr.savedAt) || "";
          const savedStr = updatedTs ? new Date(updatedTs).toLocaleString() : "";
          const slots = (cr && cr.slots) || (lr && lr.playerSlots) || [];
          const activeRoom =
            remoteRooms.find((rm) => rm && rm.run_id === runId) ||
            (cr && Number(cr.activeRoomPort) > 0 ? { game_port: Number(cr.activeRoomPort) } : null);
          const activePort = activeRoom ? Number(activeRoom.game_port) : 0;
          const myPeerSlot =
            slots.find((s, idx) => {
              const sIdx = Number(s && (s.slotIdx ?? s.slotIndex ?? idx));
              return s && ((myUserId && s.userId === myUserId) || (myToken && s.token === myToken) || (cr && Number(cr.mySlotIdx) === sIdx));
            }) || null;

          let actionsHtml = "";
          if (groupType === "joined") {
            const joinBtnHtml =
              activePort > 0
                ? `<button class="em-btn primary sm" type="button" data-join-room="${activePort}">Join Room #${activePort}</button>`
                : `<span class="em-badge sm" title="Host is not currently online">Host Offline</span>`;
            const localResumeBtn = lr
              ? `<button class="em-btn sm" type="button" data-resume-run="${escapeHtml(runId)}" title="Re-host using local snapshot">Re-Host</button>`
              : cr
                ? `<button class="em-btn sm" type="button" data-resume-cloud-peer="${escapeHtml(runId)}" title="Load your saved slot offline">Load Slot</button>`
                : "";
            actionsHtml = `${joinBtnHtml}${localResumeBtn}`;
          } else {
            const resumeAttr = cr && !lr ? `data-resume-cloud-run="${escapeHtml(runId)}"` : `data-resume-run="${escapeHtml(runId)}"`;
            const resumeLabel = groupType === "hosted" ? "Resume & Host" : "Resume";
            const exportBtn =
              includeManageBtns && lr
                ? `<button class="em-btn sm" type="button" data-export-run="${escapeHtml(runId)}">Export</button>`
                : "";
            const delAttr = cr ? `data-delete-cloud-run="${escapeHtml(runId)}"` : `data-delete-run="${escapeHtml(runId)}"`;
            const delBtn = includeManageBtns
              ? `<button class="em-btn danger sm" type="button" ${delAttr}>✕</button>`
              : "";
            actionsHtml = `<button class="em-btn primary sm" type="button" ${resumeAttr}>${resumeLabel}</button>${exportBtn}${delBtn}`;
          }

          const peerNote =
            groupType === "joined" && myPeerSlot
              ? ` • Your Party: ${escapeHtml(summarizePartyShort(myPeerSlot.party))}`
              : cr && cr.hostUsername
                ? ` • Host: @${escapeHtml(cr.hostUsername)}`
                : "";

          return `
            <div class="em-list-row">
              <div style="min-width:0;flex:1;">
                <div class="gba-font" style="font-weight:700;color:#154c3e;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">
                  ${escapeHtml(name)}
                </div>
                <div style="font-size:11px;color:#55626d;">
                  Seed: ${escapeHtml(seed)} • ${badgeCount}/8 Badges${peerNote}${savedStr ? ` • ${escapeHtml(savedStr)}` : ""}
                </div>
                ${groupType !== "solo" ? buildSlotChipsHtml(slots, myUserId) : ""}
              </div>
              <div style="display:flex;gap:4px;flex-shrink:0;align-items:center;flex-wrap:wrap;">
                ${actionsHtml}
              </div>
            </div>
          `;
        })
        .join("");
    };

    const buildGroupedHtml = (includeManageBtns) => {
      return (
        `<div class="save-group-header"><span>Hosted Runs</span><span>${hostedEntries.length}</span></div>` +
        renderGroupRows(hostedEntries, "No hosted multiplayer runs yet.", includeManageBtns, "hosted") +
        `<div class="save-group-header"><span>Joined Runs (Peer)</span><span>${joinedEntries.length}</span></div>` +
        renderGroupRows(joinedEntries, "No joined peer runs yet.", includeManageBtns, "joined") +
        `<div class="save-group-header"><span>Solo Runs (Local IndexedDB)</span><span>${soloEntries.length}</span></div>` +
        renderGroupRows(soloEntries, "No solo local runs yet.", includeManageBtns, "solo")
      );
    };

    if (resumeEl) setHtmlIfChanged(resumeEl, buildGroupedHtml(false));
    if (savesEl) setHtmlIfChanged(savesEl, buildGroupedHtml(true));
  }

  function renderSavesModal() {
    const s = PokemonNet.state;
    const badgeCount = s.world.badges.filter(Boolean).length;
    const gameOverSuffix = s.runConfig && s.runConfig.gameOver ? " — GAME OVER" : "";
    const titleEl = $("active-run-summary-title");
    if (titleEl) {
      titleEl.textContent = `Active Run: ${s.runConfig.roomName} (Seed: ${s.runConfig.seed}) — ${badgeCount}/8 Badges${gameOverSuffix}`;
    }
    const subEl = $("active-run-summary-sub");
    if (subEl) {
      subEl.textContent = PokemonNet.isNetworkHost()
        ? "You are Host Authority: saving mirrors compact RunSnapshots to all clients' IndexedDB."
        : PokemonNet.isClient()
          ? "Client Mirror: your local IndexedDB caches synced snapshots so you can re-host next session."
          : "Solo / Local Mode: stored in browser IndexedDB + exportable to JSON.";
    }
    renderSavedRunsLists();
  }

  function renderTradeModal() {
    const state = PokemonNet.state;
    const tr = state.trade;
    const myIdx = state.connection.mySlotIndex;
    const me = state.players[myIdx];

    const partnerSelect = $("trade-partner-select");
    if (partnerSelect) {
      const peers = state.players.filter((p) => p.connected && p.slotIndex !== myIdx);
      if (peers.length === 0) {
        setHtmlIfChanged(partnerSelect, `<option value="">No online peers in room</option>`);
      } else {
        setHtmlIfChanged(
          partnerSelect,
          peers
            .map((p) => {
              const slotNum = Number(p.slotIndex) || 0;
              const tutTag = !p.tutorialComplete ? " [TUTORIAL]" : "";
              return `<option value="${slotNum}" ${slotNum === Number(tr.partnerSlotIndex) ? "selected" : ""}>P${slotNum + 1} ${escapeHtml(p.name)}${tutTag}</option>`;
            })
            .join("")
        );
      }
    }

    const tradeStartBtn = $("trade-start-btn");
    if (tradeStartBtn) {
      const selSlot = partnerSelect && partnerSelect.value !== "" ? Number(partnerSelect.value) : null;
      const tradeGate = getUiFeatureGateReason(state, selSlot);
      tradeStartBtn.disabled = Boolean(tradeGate);
      tradeStartBtn.title = tradeGate || "Request Atomic Quick-Trade";
    }

    setTextIfChanged($("trade-status-banner"), tr.statusText || "Ready");
    setTextIfChanged($("trade-my-balance"), `¥${Number(me ? me.money : 0).toLocaleString()}`);

    const monSel = $("trade-my-mon-select");
    if (monSel && me) {
      const curIdx = tr.myOffer.partyIndex;
      const opts = [`<option value="-1">— No Pokémon Offered —</option>`];
      me.party.forEach((m, idx) => {
        const evoNote = TRADE_EVOLVABLE.has(m.species) ? " ★Trade Evo!" : "";
        opts.push(
          `<option value="${Number(idx)}" ${idx === curIdx ? "selected" : ""}>${escapeHtml(m.nickname)} (${escapeHtml(m.species)} Lv.${Number(m.level) || 0}) [${escapeHtml(m.linkId || "SOLO")}]${evoNote}</option>`
        );
      });
      setHtmlIfChanged(monSel, opts.join(""));
      const chosen = curIdx >= 0 ? me.party[curIdx] : null;
      setTextIfChanged(
        $("trade-my-mon-preview"),
        chosen
          ? `LinkID ${chosen.linkId || "SOLO"} travels with Pokémon • Held: ${chosen.heldItem || "None"}`
          : "Optional: pick a party Pokémon to trade."
      );
    }

    const itemSel = $("trade-my-item-select");
    if (itemSel && me) {
      setHtmlIfChanged(
        itemSel,
        (me.bagItems || [])
          .map((it) => `<option value="${escapeHtml(it.id)}">${escapeHtml(it.name)} (x${Number(it.qty)})</option>`)
          .join("")
      );
    }

    const myItemsList = $("trade-my-items-list");
    if (myItemsList) {
      setHtmlIfChanged(
        myItemsList,
        pendingTradeItems
          .map(
            (it, idx) => `
        <span class="em-badge ok" style="gap:4px;">
          ${Number(it.qty)}x ${escapeHtml(it.name)}
          <button type="button" data-remove-trade-item="${idx}" style="background:none;border:none;color:#fff;cursor:pointer;padding:0 2px;">✕</button>
        </span>
      `
          )
          .join("")
      );
    }

    // Cards & Locks
    $("trade-my-card").classList.toggle("locked", tr.myLocked);
    $("trade-partner-card").classList.toggle("locked", tr.partnerLocked);
    $("trade-my-lock-badge").textContent = tr.myLocked ? "LOCKED ✓" : "EDITING";
    $("trade-my-lock-badge").className = `em-badge ${tr.myLocked ? "ok" : ""}`;
    $("trade-partner-lock-badge").textContent = tr.partnerLocked ? "LOCKED ✓" : tr.active ? "UNLOCKED" : "WAITING";
    $("trade-partner-lock-badge").className = `em-badge ${tr.partnerLocked ? "ok" : ""}`;
    $("trade-lock-btn").textContent = tr.myLocked ? "🔓 Unlock Offer" : "🔒 Lock My Offer";
    const gameOver = Boolean(state.runConfig && state.runConfig.gameOver);
    $("trade-lock-btn").disabled = !tr.active || gameOver;
    $("trade-confirm-btn").disabled = gameOver || !(tr.active && tr.myLocked && tr.partnerLocked && !tr.myConfirmed);

    // Partner Offer Render
    $("trade-partner-name-label").textContent = tr.partnerName ? `${tr.partnerName.toUpperCase()}'S OFFER` : "PARTNER OFFER";
    const pMonBox = $("trade-partner-mon-box");
    if (tr.partnerOffer && tr.partnerOffer.mon) {
      const pm = tr.partnerOffer.mon;
      pMonBox.innerHTML = `
        <strong>${escapeHtml(pm.nickname)} (${escapeHtml(pm.species)})</strong> Lv.${Number(pm.level)}
        <div style="font-size:11px;color:#1f6f5b;">${escapeHtml(pm.linkId || "SOLO")} • Held: ${escapeHtml(pm.heldItem || "None")}</div>
      `;
    } else {
      pMonBox.textContent = "No Pokémon offered.";
    }
    const pItemsBox = $("trade-partner-items-box");
    const pItems = (tr.partnerOffer && tr.partnerOffer.items) || [];
    pItemsBox.innerHTML = pItems.length
      ? pItems.map((it) => `<span class="em-badge" style="margin-right:4px;">${Number(it.qty)}x ${escapeHtml(it.name)}</span>`).join("")
      : "No Bag Items offered.";
    $("trade-partner-money-label").textContent = `¥${Number((tr.partnerOffer && tr.partnerOffer.money) || 0).toLocaleString()}`;
  }

  function showBusyBattlePrompt(payload) {
    const stack = $("toast-stack");
    if (!stack || !payload) return;
    const prev = stack.querySelector("[data-busy-battle-prompt]");
    if (prev) prev.remove();

    const el = document.createElement("div");
    el.className = "em-panel em-toast";
    el.dataset.busyBattlePrompt = "1";
    el.style.borderLeft = "5px solid #c99416";
    const reasonLine =
      payload.busyReason
        ? `${escapeHtml(payload.targetName || "Partner")} is ${escapeHtml(payload.busyReason)}.`
        : escapeHtml(formatBusyReasonText(payload.reason, payload.targetName, payload.mapName));
    el.innerHTML = `
      <div class="gba-font" style="font-size:11px;font-weight:700;color:#154c3e;">TEAMMATE BUSY</div>
      <div style="font-size:12px;margin-top:2px;">${reasonLine} Fight Solo now or wait?</div>
      <div style="display:flex;gap:6px;margin-top:8px;">
        <button class="em-btn primary sm" type="button" data-busy-act="solo">Battle Solo / AI Partner Now</button>
        <button class="em-btn sm" type="button" data-busy-act="wait">Wait for Teammate</button>
        <button class="em-btn sm" type="button" data-busy-act="dismiss">Cancel</button>
      </div>
    `;
    stack.appendChild(el);

    const autoClose = setTimeout(() => {
      if (el.isConnected) el.remove();
    }, 15000);

    el.addEventListener("click", (ev) => {
      const btn = ev.target.closest("[data-busy-act]");
      if (!btn) return;
      const act = btn.dataset.busyAct;
      clearTimeout(autoClose);
      el.remove();
      if (act === "solo") {
        global.PokemonWasmBridge._startSyncedBattleInWasm({
          mode: "solo_2v2",
          battleSeed: Math.floor(Math.random() * 0xffffffff),
        });
      } else if (act === "wait") {
        showToast("info", "Waiting for Teammate", `Will invite ${payload.targetName || "partner"} as soon as they are free.`);
        const waitTimer = setInterval(() => {
          const t = PokemonNet.state.players[payload.targetSlotIndex];
          if (!t || !t.connected) {
            clearInterval(waitTimer);
            return;
          }
          if (!t.inBattle && !t.busy) {
            clearInterval(waitTimer);
            PokemonNet.sendBattleInvite(payload.targetSlotIndex, payload.mode);
          }
        }, 400);
        setTimeout(() => clearInterval(waitTimer), 60000);
      }
      focusCanvas();
    });
  }

  function dismissTeammateBusyPrompt() {
    const stack = $("toast-stack");
    if (!stack) return;
    const prev = stack.querySelector("[data-teammate-busy-prompt]");
    if (prev) prev.remove();
  }

  function showTeammateBusyPrompt(payload) {
    const stack = $("toast-stack");
    if (!stack || !payload) return;
    dismissTeammateBusyPrompt();

    const partnerName = payload.targetName || "Partner";
    const el = document.createElement("div");
    el.className = "em-panel em-toast";
    el.dataset.teammateBusyPrompt = "1";
    el.style.borderLeft = "5px solid #c99416";
    el.innerHTML = `
      <div class="gba-font" style="font-size:11px;font-weight:700;color:#154c3e;">${escapeHtml(payload.title || "PARTNER BUSY")}</div>
      <div style="font-size:12px;margin-top:2px;">${escapeHtml(payload.message || `${partnerName} is busy.`)} Wait for them or proceed with an AI partner?</div>
      <div style="display:flex;gap:6px;margin-top:8px;flex-wrap:wrap;">
        <button class="em-btn primary sm" type="button" data-teammate-prompt-act="wait">Wait for Partner</button>
        <button class="em-btn gold sm" type="button" data-teammate-prompt-act="ai">Proceed with AI Partner</button>
      </div>
    `;
    stack.appendChild(el);

    el.addEventListener("click", (ev) => {
      const btn = ev.target.closest("[data-teammate-prompt-act]");
      if (!btn) return;
      const act = btn.dataset.teammatePromptAct;
      if (act === "wait") {
        if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge._resolveBusyTrainerPrompt === "function") {
          global.PokemonWasmBridge._resolveBusyTrainerPrompt(0);
        }
        el.style.borderLeft = "5px solid #2d9575";
        el.innerHTML = `
          <div class="gba-font" style="font-size:11px;font-weight:700;color:#154c3e;">WAITING FOR ${escapeHtml(partnerName.toUpperCase())}...</div>
          <div style="font-size:12px;margin-top:2px;">Paused before battle. Will start synced 2v2 as soon as ${escapeHtml(partnerName)} is ready.</div>
          <div style="display:flex;gap:6px;margin-top:8px;">
            <button class="em-btn gold sm" type="button" data-teammate-prompt-act="ai">Proceed with AI Partner</button>
          </div>
        `;
        focusCanvas();
      } else if (act === "ai") {
        if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge._resolveBusyTrainerPrompt === "function") {
          global.PokemonWasmBridge._resolveBusyTrainerPrompt(1);
        }
        el.remove();
        focusCanvas();
      }
    });
  }

  function openSlotClaimModal(payload) {
    if (!payload) return;
    const heading = $("slot-claim-heading");
    if (heading) {
      setTextIfChanged(heading, `${payload.roomName || "Room"} — Claim Offline Slot or Join New`);
    }
    const listEl = $("slot-claim-list");
    if (listEl) {
      const slots = Array.isArray(payload.offlineSlots) ? payload.offlineSlots : [];
      setHtmlIfChanged(
        listEl,
        slots
          .map((s) => {
            const idx = Number(s.slotIndex) || 1;
            const uname = s.username ? ` (@${s.username})` : "";
            const tutText = s.tutorialComplete ? "Tutorial Complete" : "In Tutorial";
            return `
              <div class="em-list-row">
                <div style="min-width:0;flex:1;">
                  <div class="gba-font" style="font-weight:700;color:#154c3e;">
                    P${idx + 1} ${escapeHtml(s.name || `Trainer ${idx + 1}`)}${escapeHtml(uname)}
                  </div>
                  <div style="font-size:11px;color:#55626d;">
                    ${escapeHtml(tutText)} • ${escapeHtml(summarizePartyShort(s.party))}
                  </div>
                </div>
                <button class="em-btn primary sm" type="button" data-claim-slot="${idx}">Claim P${idx + 1}</button>
              </div>
            `;
          })
          .join("")
      );
    }
    const newBtn = $("slot-claim-new-btn");
    if (newBtn) {
      newBtn.style.display = payload.hasEmptySlot ? "inline-flex" : "none";
    }
    openModal("slot-claim-modal");
  }

  function openLateJoinModal(ctx) {
    if (!ctx) return;
    pendingLateJoinCtx = ctx;
    const badgeCount = Number(ctx.badgeCount) || 0;
    const deathCount = Number(ctx.deathCount) || 0;
    const caughtRoutesCount = Number(ctx.caughtRoutesCount) || 0;
    const presetKey = ctx.nuzlockePreset || (ctx.runConfig && ctx.runConfig.nuzlockePreset) || "standard_soullink";
    const presetLabel = NUZLOCKE_LABELS[presetKey] || presetKey;
    const roomTitle = ctx.roomName || (ctx.runConfig && ctx.runConfig.roomName) || `Room #${Number(ctx.port) || 0}`;
    const hostName = ctx.hostName || "Host";

    setTextIfChanged(
      $("late-join-room-heading"),
      `${roomTitle} • Host: ${hostName} • ${presetLabel}`
    );
    const summaryEl = $("late-join-progress-summary");
    if (summaryEl) {
      summaryEl.innerHTML = `Host's Soul-Link run already has <strong>${badgeCount}/8 Badges</strong>, <strong>${deathCount} Deaths</strong>, and <strong>${caughtRoutesCount} Locked Routes</strong>.`;
    }

    const deadRoutes = Array.isArray(ctx.deadRoutes) ? ctx.deadRoutes : [];
    const graveyard = Array.isArray(ctx.graveyard) ? ctx.graveyard : [];
    const detailsBox = $("late-join-dead-routes-box");
    const detailsList = $("late-join-dead-routes-list");
    if (detailsBox && detailsList) {
      if (deadRoutes.length === 0 && graveyard.length === 0) {
        detailsBox.style.display = "none";
      } else {
        const rows = [];
        for (const dr of deadRoutes) {
          const label = (dr && (dr.routeName || dr.routeId)) || String(dr || "Locked Route");
          const st = getRouteLockStatusInfo(typeof dr === "object" ? dr : { dead: true });
          const linkStr = dr && dr.linkId ? ` [${dr.linkId}]` : "";
          rows.push(
            `<div class="em-list-row"><span><strong>${escapeHtml(label)}</strong>${escapeHtml(linkStr)}</span><span class="route-lock-status ${st.key}">${st.label}</span></div>`
          );
        }
        for (const g of graveyard.slice(0, 8)) {
          if (!g) continue;
          rows.push(
            `<div class="em-list-row"><span><strong>${escapeHtml(g.nickname || g.species)}</strong> (${escapeHtml(g.species)}) • ${escapeHtml(g.originMap || "Hoenn")}</span><span style="font-size:11px;color:#9f1239;">${escapeHtml(g.cause || "Fainted")}</span></div>`
          );
        }
        setHtmlIfChanged(detailsList, rows.join(""));
        detailsBox.style.display = "block";
      }
    }
    openModal("late-join-modal");
  }

  function renderAll(state, reason, payload) {
    if (reason === "toast" && payload) {
      showToast(payload.kind, payload.title, payload.message);
      return;
    }
    if (reason === "teammate_busy" && payload) {
      const msg = payload.message || formatBusyReasonText(payload.reason, payload.targetName, payload.mapName);
      showToast("gold", payload.title || "AI / Solo Fallback", msg);
      return;
    }
    if (reason === "teammate_busy_prompt" && payload) {
      showTeammateBusyPrompt(payload);
      return;
    }
    if (reason === "teammate_busy_resolved") {
      dismissTeammateBusyPrompt();
      focusCanvas();
      return;
    }
    if (reason === "busy_battle_prompt" && payload) {
      showBusyBattlePrompt(payload);
      return;
    }
    if (reason === "slot_claim_required" && payload) {
      openSlotClaimModal(payload);
      return;
    }
    if (reason === "late_join_confirm_needed" && payload) {
      openLateJoinModal({ source: "welcome", ...payload });
      return;
    }
    if (reason === "auth") {
      renderAuthUi(state);
      if ($("saves-modal") && $("saves-modal").classList.contains("open")) renderSavedRunsLists();
      if ($("rooms-modal") && $("rooms-modal").classList.contains("open")) renderSavedRunsLists();
      return;
    }
    if (reason === "saves") {
      if ($("saves-modal") && $("saves-modal").classList.contains("open")) renderSavesModal();
      if ($("rooms-modal") && $("rooms-modal").classList.contains("open")) renderSavedRunsLists();
      return;
    }
    if (reason === "join_rejected" && payload) {
      pendingLateJoinCtx = null;
      if ($("late-join-modal") && $("late-join-modal").classList.contains("open")) {
        $("late-join-modal").classList.remove("open");
      }
      const errMsg = payload.message || payload.reason || "Join rejected by room.";
      const errBox = $("rooms-join-error");
      const errText = $("rooms-join-error-text");
      if (errBox && errText) {
        setTextIfChanged(errText, errMsg);
        errBox.style.display = "block";
      }
      showToast("danger", "Cannot Join Room", errMsg);
      return;
    }
    if (reason === "soullink_death_parity" && payload) {
      const msg = payload.summary || "Soul-Link pairing triggered route death parity.";
      showToast("danger", "Soul-Link Death Parity", msg);
      return;
    }
    if (reason === "dead_route_reject" && payload) {
      const msg =
        payload.message ||
        `Catch rejected on dead/locked route (${payload.routeName || "Hoenn"}).`;
      showToast("danger", "Dead Route Locked", msg);
      return;
    }
    if (reason === "trade_open") {
      if (getUiFeatureGateReason(state)) return;
      pendingTradeItems = [];
      openModal("trade-modal");
      return;
    }
    if (reason === "rooms") {
      setTextIfChanged($("room-count-num"), String((PokemonNet.remoteRooms || []).length));
      if ($("rooms-modal") && $("rooms-modal").classList.contains("open")) {
        renderRoomBrowserList();
      }
      return;
    }
    if (state && state.connection && (state.connection.status === "hosting" || state.connection.status === "connected")) {
      clearRoomsJoinError();
    }
    renderTopbar(state);
    renderCoopBar(state);
    renderSoulLinkSidebar(state);
    renderBattleInviteToast(state);
    renderCutsceneBanner(state);
    if ($("trade-modal").classList.contains("open")) renderTradeModal();
    if ($("saves-modal").classList.contains("open")) renderSavesModal();
  }

  // --- Preset Sync Helpers for Room Creation ---
  function applyRandomizerPresetToForm(presetName) {
    const p = PokemonConstants.RANDOMIZER_PRESETS[presetName];
    if (!p) return;
    $("rnd-wild").checked = p.wildEncounters;
    $("rnd-starters").checked = p.startersAndStatics;
    $("rnd-trainers").checked = p.trainers2v2;
    $("rnd-items").checked = p.itemsAndMarts;
    $("rnd-abilities").checked = p.abilities;
    $("rnd-ban-wg").checked = p.banWonderGuard;
    $("rnd-movesets").checked = p.movesets;
    $("rnd-types").checked = p.types;
    $("rnd-evos").checked = p.evolutions;
    $("rnd-gym-themes").checked = p.preserveGymThemes;
    $("rnd-bst").checked = p.similarBst;
  }

  function normalizeSeedInput(raw) {
    return String(raw || "")
      .replace(/[^\x20-\x7e]/g, "")
      .trim()
      .slice(0, 31);
  }

  function syncHostRandomizerPresetToForm(resetToDefault) {
    const presetSel = $("setup-randomizer-preset");
    if (!presetSel) return;
    if (resetToDefault || !PokemonConstants.RANDOMIZER_PRESETS[presetSel.value]) {
      const cfgPreset = PokemonNet.state.runConfig && PokemonNet.state.runConfig.randomizerPreset;
      presetSel.value = PokemonConstants.RANDOMIZER_PRESETS[cfgPreset] ? cfgPreset : "balanced";
      applyRandomizerPresetToForm(presetSel.value);
    }
  }

  function applyNuzlockePresetToForm(presetName) {
    const p = PokemonConstants.NUZLOCKE_PRESETS[presetName];
    if (!p) return;
    $("nz-enabled").checked = p.enabled;
    $("nz-dupes").checked = p.dupesClause;
    $("nz-shiny").checked = p.shinyClause;
    $("nz-level-caps").checked = p.levelCaps;
    $("nz-set-mode").checked = p.setMode;
    $("nz-no-items").checked = p.noBattleItems;
    $("nz-whiteout").checked = p.whiteoutGameOver;
  }

  function collectHostRunConfigFromForm() {
    const rawSeed = $("setup-seed-input") ? $("setup-seed-input").value : "";
    const normalizedSeed = normalizeSeedInput(rawSeed) || normalizeSeedInput(PokemonNet.state.runConfig.seed);
    if ($("setup-seed-input") && normalizedSeed) {
      $("setup-seed-input").value = normalizedSeed;
    }
    return {
      roomName: $("setup-room-name").value.trim() || `${$("player-name-input").value}'s Hoenn Run`,
      roomPort: parseInt($("setup-room-port").value, 10) || 27462,
      seed: normalizedSeed,
      soulLinkMode: $("setup-soullink-mode").value,
      battleAutoPull: $("setup-battle-autopull").checked,
      freshReset: Boolean($("setup-fresh-reset") && $("setup-fresh-reset").checked),
      randomizerPreset: $("setup-randomizer-preset").value,
      randomizerToggles: {
        wildEncounters: $("rnd-wild").checked,
        startersAndStatics: $("rnd-starters").checked,
        trainers2v2: $("rnd-trainers").checked,
        itemsAndMarts: $("rnd-items").checked,
        abilities: $("rnd-abilities").checked,
        banWonderGuard: $("rnd-ban-wg").checked,
        movesets: $("rnd-movesets").checked,
        types: $("rnd-types").checked,
        evolutions: $("rnd-evos").checked,
        preserveGymThemes: $("rnd-gym-themes").checked,
        similarBst: $("rnd-bst").checked,
      },
      nuzlockePreset: $("setup-nuzlocke-preset").value,
      nuzlockeToggles: {
        enabled: $("nz-enabled").checked,
        dupesClause: $("nz-dupes").checked,
        shinyClause: $("nz-shiny").checked,
        levelCaps: $("nz-level-caps").checked,
        setMode: $("nz-set-mode").checked,
        noBattleItems: $("nz-no-items").checked,
        whiteoutGameOver: $("nz-whiteout").checked,
      },
      speedMultiplier: PokemonNet.state.runConfig.speedMultiplier || 1,
    };
  }

  // --- Phase 0 GBA Preview Canvas Loop (auto-stops when Emscripten boots) ---
  function startGbaPreviewCanvas() {
    const canvas = $("canvas");
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    let tick = 0;
    let lastTs = performance.now();

    function frame(now) {
      if (previewAnimStop || PokemonNet.state.wasmReady) return;
      const dt = Math.min(0.1, (now - lastTs) / 1000);
      lastTs = now;
      const speed = PokemonNet.state.runConfig.speedMultiplier || 1;
      tick += dt * 60 * speed;

      // Authentic 240x160 Emerald Sky/Hoenn Route backdrop
      const skyGrad = ctx.createLinearGradient(0, 0, 0, 100);
      skyGrad.addColorStop(0, "#154c3e");
      skyGrad.addColorStop(0.55, "#2d9575");
      skyGrad.addColorStop(1, "#84f0c4");
      ctx.fillStyle = skyGrad;
      ctx.fillRect(0, 0, 240, 160);

      // Rolling pixelated Hoenn hills
      ctx.fillStyle = "#1f6f5b";
      for (let x = 0; x < 240; x += 6) {
        const h = 34 + Math.round(Math.sin((x + tick * 0.25) * 0.045) * 8);
        ctx.fillRect(x, 112 - h, 6, h);
      }

      // Tall grass ground strip
      ctx.fillStyle = "#2b7a4b";
      ctx.fillRect(0, 106, 240, 54);
      ctx.fillStyle = "#3ca862";
      for (let x = 0; x < 240; x += 8) {
        const sway = Math.round(Math.sin((tick * 0.08) + x) * 1.2);
        ctx.fillRect(x + sway, 108, 4, 5);
        ctx.fillRect(x - sway + 3, 122, 4, 5);
      }

      // Route path
      ctx.fillStyle = "#eae2c0";
      ctx.fillRect(0, 134, 240, 18);
      ctx.fillStyle = "#d1c59c";
      ctx.fillRect(0, 134, 240, 2);
      ctx.fillRect(0, 150, 240, 2);

      // Connected players walking sprites (dots with GBA caps)
      const activePlayers = PokemonNet.state.players.filter((p) => p.connected);
      activePlayers.forEach((p, idx) => {
        const px = Math.round(((tick * 0.8 + idx * 52) % 210) + 12);
        const bob = Math.round(Math.abs(Math.sin(tick * 0.14 + idx)) * 2);
        const py = 136 - bob;
        const colors = ["#e63946", "#3a86ff", "#f4c430", "#9b5de5"];
        ctx.fillStyle = "#202830";
        ctx.fillRect(px - 1, py - 1, 10, 12);
        ctx.fillStyle = colors[p.slotIndex % 4];
        ctx.fillRect(px, py, 8, 5);
        ctx.fillStyle = "#f8d4b4";
        ctx.fillRect(px + 1, py + 5, 6, 4);
        ctx.fillStyle = "#1f6f5b";
        ctx.fillRect(px + 1, py + 9, 6, 2);
        ctx.fillStyle = "#202830";
        ctx.font = "bold 7px monospace";
        ctx.fillText(`P${p.slotIndex + 1}`, px - 1, py - 3);
      });

      // Top GBA Emblem Box
      ctx.fillStyle = "rgba(11, 36, 29, 0.88)";
      ctx.fillRect(14, 14, 212, 44);
      ctx.strokeStyle = "#f8f4dc";
      ctx.lineWidth = 2;
      ctx.strokeRect(14, 14, 212, 44);
      ctx.fillStyle = "#f4c430";
      ctx.font = "bold 10px monospace";
      ctx.fillText("POKEMON EMERALD MP", 24, 30);
      ctx.fillStyle = "#84f0c4";
      ctx.font = "8px monospace";
      ctx.fillText(`SEED: ${PokemonNet.state.runConfig.seed}  SPD: ${speed}x`, 24, 44);
      ctx.fillStyle = "#f8f4dc";
      ctx.fillText(`LINKED: ${activePlayers.length}/4 TRAINERS`, 24, 53);

      requestAnimationFrame(frame);
    }

    requestAnimationFrame(frame);
  }

  // --- WASM Engine + IndexedDB Asset Boot ---
  async function bootGameOrPreview() {
    const statusEl = $("loading-status");
    const fillEl = $("progress-fill");
    const overlayEl = $("loading-overlay");

    try {
      let assetPack = null;
      try {
        assetPack = await PokemonAssetLoader.loadChunkedAssetPack(
          (msg) => {
            if (statusEl) statusEl.textContent = msg;
          },
          (pct) => {
            if (fillEl) fillEl.style.width = `${pct}%`;
          }
        );
      } catch (chunkErr) {
        console.warn("Asset chunk check skipped/failed:", chunkErr);
      }

      if (statusEl) statusEl.textContent = "Probing Emscripten pokeemerald-expansion core (/index.js)...";
      const probe = await fetch("/index.js", { method: "HEAD", cache: "no-cache" }).catch(() => null);
      if (!probe || !probe.ok) {
        if (fillEl) fillEl.style.width = "100%";
        if (statusEl) statusEl.textContent = "Relay & Emerald Companion HUD Ready (Preview Mode)";
        setTimeout(() => {
          if (overlayEl) overlayEl.classList.add("hidden");
          startGbaPreviewCanvas();
          handleAutoInviteJoin();
        }, 280);
        return;
      }

      if (statusEl) statusEl.textContent = "Booting Emscripten WebAssembly engine...";
      if (fillEl) fillEl.style.width = "75%";

      let preloadedWasm =
        assetPack && ((assetPack.files && assetPack.files["index.wasm"]) || assetPack.bytes)
          ? (assetPack.files && assetPack.files["index.wasm"]) || assetPack.bytes
          : null;

      if (preloadedWasm) {
        const wasmProbe = await fetch("/index.wasm", { method: "HEAD", cache: "no-cache" }).catch(() => null);
        if (wasmProbe && wasmProbe.ok) {
          const liveType = (wasmProbe.headers.get("content-type") || "").toLowerCase();
          const liveLen = Number(wasmProbe.headers.get("content-length") || 0);
          const isWasmLike = !liveType.includes("json") && !liveType.includes("text/");
          if (isWasmLike && liveLen > 0 && liveLen !== preloadedWasm.byteLength) {
            console.warn(
              `Preloaded chunked index.wasm size mismatch (${preloadedWasm.byteLength} vs live ${liveLen}); using live /index.wasm`
            );
            preloadedWasm = null;
          }
        }
      }

      const jsTag = (probe.headers && (probe.headers.get("etag") || probe.headers.get("last-modified"))) || "";
      const jsCacheKey = jsTag ? encodeURIComponent(jsTag.replace(/"/g, "")) : String(Date.now());

      global.Module = {
        canvas: $("canvas"),
        preloadedAssetPack: preloadedWasm,
        setStatus(text) {
          if (statusEl && text) statusEl.textContent = text;
        },
        onRuntimeInitialized() {
          previewAnimStop = true;
          if (fillEl) fillEl.style.width = "100%";
          if (overlayEl) overlayEl.classList.add("hidden");
          PokemonWasmBridge.onWasmReady(global.Module);
          syncAudioContextMuteState();
          focusCanvas();
          handleAutoInviteJoin();
        },
      };

      if (preloadedWasm) {
        global.Module.instantiateWasm = (imports, successCallback) => {
          WebAssembly.instantiate(preloadedWasm, imports)
            .then((result) => {
              successCallback(result.instance, result.module);
            })
            .catch(async (err) => {
              console.warn("Preloaded WASM instantiation failed, falling back to /index.wasm:", err);
              try {
                const res = await fetch("/index.wasm", { cache: "no-cache" });
                if (!res.ok) throw new Error(`HTTP ${res.status} fetching /index.wasm`);
                const fallbackBuf = await res.arrayBuffer();
                const fallbackResult = await WebAssembly.instantiate(fallbackBuf, imports);
                successCallback(fallbackResult.instance, fallbackResult.module);
              } catch (fallbackErr) {
                console.error("WASM fallback instantiation failed:", fallbackErr);
                if (overlayEl) overlayEl.classList.add("hidden");
                startGbaPreviewCanvas();
                handleAutoInviteJoin();
              }
            });
          return {};
        };
      }

      const script = document.createElement("script");
      script.src = `/index.js?v=${jsCacheKey}`;
      script.async = true;
      script.onerror = () => {
        if (overlayEl) overlayEl.classList.add("hidden");
        startGbaPreviewCanvas();
        handleAutoInviteJoin();
      };
      document.body.appendChild(script);
    } catch (err) {
      console.error("Boot fallback to preview:", err);
      if (overlayEl) overlayEl.classList.add("hidden");
      startGbaPreviewCanvas();
    }
  }

  function handleAutoInviteJoin() {
    if (!global.location || !global.location.search) return;
    const roomParam = new URLSearchParams(global.location.search).get("room");
    const port = Number(roomParam);
    if (port > 0) {
      setTimeout(() => {
        PokemonNet.joinRoom(port);
      }, 300);
    }
  }

  // --- Wire DOM Events ---
  function bindEvents() {
    const nameInput = $("player-name-input");
    if (nameInput) {
      nameInput.value = PokemonNet.state.players[0].name;
      nameInput.addEventListener("change", (e) => {
        PokemonNet.setPlayerName(e.target.value);
        focusCanvas();
      });
    }

    const relayInput = $("relay-url-input");
    if (relayInput) {
      relayInput.value = global.POKEMON_RELAY_URL || "";
    }

    $("setup-room-name").value = PokemonNet.state.runConfig.roomName;
    $("setup-seed-input").value = PokemonNet.state.runConfig.seed;
    syncHostRandomizerPresetToForm(true);
    for (const lbl of document.querySelectorAll(".em-check[title]")) {
      const input = lbl.querySelector("input");
      if (input && !input.title) {
        input.title = lbl.title;
      }
    }

    // Host-Only Speed Controls
    const speedGroup = $("speed-control-group");
    if (speedGroup) {
      speedGroup.addEventListener("click", (e) => {
        const btn = e.target.closest(".speed-btn");
        if (!btn) return;
        PokemonNet.setGlobalSpeed(Number(btn.dataset.speed));
        focusCanvas();
      });
    }

    // Copy Invite
    $("copy-invite-btn").addEventListener("click", () => {
      const url = PokemonNet.buildInviteUrl();
      copyText(url);
      const btn = $("copy-invite-btn");
      btn.textContent = "✅ Link Copied!";
      clearTimeout(global._pkCopyTimer);
      global._pkCopyTimer = setTimeout(() => {
        btn.textContent = "🔗 Copy Invite";
      }, 2200);
      focusCanvas();
    });

    // Topbar Modals & Toggles
    $("open-rooms-btn").addEventListener("click", () => openModal("rooms-modal"));
    $("open-trade-btn").addEventListener("click", () => {
      const gate = getUiFeatureGateReason(PokemonNet.state);
      if (gate) {
        showToast("warn", "Quick-Trade Locked", gate);
        return;
      }
      openModal("trade-modal");
    });
    $("open-saves-btn").addEventListener("click", () => {
      if ($("reset-new-game-confirm")) $("reset-new-game-confirm").style.display = "none";
      openModal("saves-modal");
    });
    $("toggle-sidebar-btn").addEventListener("click", toggleSidebar);
    $("audio-mute-btn").addEventListener("click", () => {
      audioMuted = !audioMuted;
      if (!audioMuted && audioVolume <= 0) {
        audioVolume = lastNonZeroVolume > 0 ? lastNonZeroVolume : 80;
      }
      applyAudioMuteState();
      focusCanvas();
    });
    const volumeSlider = $("audio-volume-slider");
    if (volumeSlider) {
      const handleVolumeChange = (e) => {
        const nextVol = Math.max(0, Math.min(100, Math.round(Number(e.target.value) || 0)));
        if (nextVol > 0) {
          lastNonZeroVolume = nextVol;
          audioVolume = nextVol;
          audioMuted = false;
        } else {
          audioVolume = 0;
          audioMuted = true;
        }
        applyAudioMuteState();
      };
      volumeSlider.addEventListener("input", handleVolumeChange);
      volumeSlider.addEventListener("change", () => {
        focusCanvas();
      });
    }
    $("fullscreen-btn").addEventListener("click", toggleFullscreen);
    $("hide-hud-btn").addEventListener("click", () => toggleHud(true));
    $("show-hud-pill").addEventListener("click", () => toggleHud(false));
    $("dismiss-preview-pill-btn").addEventListener("click", () => $("wasm-preview-pill").classList.add("hidden"));

    // Modal Close Buttons & Backdrop Click
    document.addEventListener("click", (e) => {
      const closeBtn = e.target.closest("[data-close-modal]");
      if (closeBtn) {
        closeModal(closeBtn.dataset.closeModal);
        return;
      }
      if (e.target.classList && e.target.classList.contains("em-modal-backdrop")) {
        closeModal(e.target.id);
      }
    });

    // Rooms Modal Tabs
    for (const tab of document.querySelectorAll("[data-rooms-tab]")) {
      tab.addEventListener("click", () => {
        const target = tab.dataset.roomsTab;
        for (const t of document.querySelectorAll("[data-rooms-tab]")) {
          t.classList.toggle("active", t === tab);
        }
        $("rooms-tab-browse").style.display = target === "browse" ? "block" : "none";
        $("rooms-tab-create").style.display = target === "create" ? "block" : "none";
      });
    }

    $("switch-to-create-btn").addEventListener("click", () => {
      syncHostRandomizerPresetToForm(false);
      if ($("setup-fresh-reset")) {
        $("setup-fresh-reset").checked = true;
      }
      document.querySelector('[data-rooms-tab="create"]').click();
    });
    $("refresh-rooms-btn").addEventListener("click", () => {
      clearRoomsJoinError();
      PokemonNet.refreshRooms();
    });
    $("disconnect-room-btn").addEventListener("click", () => PokemonNet.disconnect());
    $("direct-join-btn").addEventListener("click", () => {
      const port = parseInt($("direct-room-port-input").value, 10) || 27462;
      attemptJoinRoomPort(port);
    });
    $("save-relay-btn").addEventListener("click", () => {
      PokemonNet.setRelayUrl($("relay-url-input").value);
      showToast("info", "Relay Updated", global.POKEMON_WS_URL);
    });
    $("randomize-seed-btn").addEventListener("click", () => {
      const words = ["RAYQUAZA", "EMERALD", "HOENN", "METEOR", "LATIAS", "KYOGRE", "SOOTOPOLIS"];
      $("setup-seed-input").value = `${words[Math.floor(Math.random() * words.length)]}-${Math.floor(1000 + Math.random() * 8999)}`.slice(0, 31);
      if ($("setup-fresh-reset")) {
        $("setup-fresh-reset").checked = true;
      }
    });
    $("setup-randomizer-preset").addEventListener("change", (e) => applyRandomizerPresetToForm(e.target.value));
    $("setup-nuzlocke-preset").addEventListener("change", (e) => applyNuzlockePresetToForm(e.target.value));

    $("launch-host-room-btn").addEventListener("click", () => {
      clearRoomsJoinError();
      const cfg = collectHostRunConfigFromForm();
      PokemonNet.hostRoom(cfg);
      closeModal("rooms-modal");
    });

    async function handleSavedRunsAction(e, modalToClose) {
      const joinBtn = e.target.closest("[data-join-room]");
      if (joinBtn) {
        if (joinBtn.disabled) return;
        attemptJoinRoomPort(joinBtn.dataset.joinRoom);
        return true;
      }
      const resBtn = e.target.closest("[data-resume-run]");
      if (resBtn) {
        const runs = await PokemonNet.listSavedRuns();
        const found = runs.find((r) => r.runId === resBtn.dataset.resumeRun);
        if (found) {
          await PokemonNet.hostRoom(null, found);
          closeModal(modalToClose);
        }
        return true;
      }
      const resCloudBtn = e.target.closest("[data-resume-cloud-run]");
      if (resCloudBtn) {
        const runId = resCloudBtn.dataset.resumeCloudRun;
        const cloudRun = (PokemonNet.cloudRuns && PokemonNet.cloudRuns.hostedRuns || []).find((r) => r.runId === runId);
        if (cloudRun) {
          const snap = await PokemonNet.buildSnapshotFromCloudRun(cloudRun, 0);
          if (snap) {
            await PokemonNet.hostRoom(null, snap);
            closeModal(modalToClose);
          }
        }
        return true;
      }
      const resCloudPeerBtn = e.target.closest("[data-resume-cloud-peer]");
      if (resCloudPeerBtn) {
        const runId = resCloudPeerBtn.dataset.resumeCloudPeer;
        const cloudRun = (PokemonNet.cloudRuns && PokemonNet.cloudRuns.joinedRuns || []).find((r) => r.runId === runId);
        if (cloudRun) {
          const myIdx = Number.isInteger(Number(cloudRun.mySlotIdx)) ? Number(cloudRun.mySlotIdx) : 1;
          const snap = await PokemonNet.buildSnapshotFromCloudRun(cloudRun, myIdx);
          if (snap) {
            await PokemonNet.applySnapshotLocal(snap, false);
            closeModal(modalToClose);
            showToast("ok", "Slot Loaded", `Loaded your saved Trainer Slot for ${cloudRun.title || runId}.`);
          }
        }
        return true;
      }
      const exportBtn = e.target.closest("[data-export-run]");
      if (exportBtn) {
        const runs = await PokemonNet.listSavedRuns();
        const found = runs.find((r) => r.runId === exportBtn.dataset.exportRun);
        if (found) downloadJson(found, `pokemon_emerald_run_${found.runId}.json`);
        return true;
      }
      const delCloudBtn = e.target.closest("[data-delete-cloud-run]");
      if (delCloudBtn) {
        const runId = delCloudBtn.dataset.deleteCloudRun;
        await PokemonNet.deleteCloudRun(runId);
        await PokemonNet.deleteSavedRun(runId);
        renderSavesModal();
        renderSavedRunsLists();
        return true;
      }
      const delBtn = e.target.closest("[data-delete-run]");
      if (delBtn) {
        await PokemonNet.deleteSavedRun(delBtn.dataset.deleteRun);
        renderSavesModal();
        renderSavedRunsLists();
        return true;
      }
      return false;
    }

    // Rooms list & Resume picker delegation
    $("rooms-modal").addEventListener("click", async (e) => {
      await handleSavedRunsAction(e, "rooms-modal");
    });

    // Sidebar Tabs & LinkID Highlighting
    for (const tab of document.querySelectorAll("[data-sidebar-tab]")) {
      tab.addEventListener("click", () => {
        currentSidebarTab = tab.dataset.sidebarTab;
        renderSoulLinkSidebar(PokemonNet.state);
      });
    }

    $("sidebar-content").addEventListener("click", (e) => {
      const slotEl = e.target.closest("[data-link-id]");
      if (!slotEl) return;
      const lid = slotEl.dataset.linkId;
      PokemonNet.state.highlightedLinkId = PokemonNet.state.highlightedLinkId === lid ? null : lid || null;
      renderSoulLinkSidebar(PokemonNet.state);
    });

    // Co-op Bar Actions
    $("hoenn-badges-strip").addEventListener("click", (e) => {
      const btn = e.target.closest("[data-toggle-badge]");
      if (!btn || !PokemonNet.isHost()) return;
      const idx = Number(btn.dataset.toggleBadge);
      if (!PokemonNet.state.world.badges[idx]) {
        PokemonNet.unlockBadge(idx);
      } else {
        PokemonNet.state.world.badges[idx] = false;
        renderCoopBar(PokemonNet.state);
      }
    });

    $("late-join-confirm-btn").addEventListener("click", () => {
      const ctx = pendingLateJoinCtx;
      pendingLateJoinCtx = null;
      closeModal("late-join-modal");
      if (!ctx) return;
      if (ctx.source === "browser") {
        PokemonNet.joinRoom(ctx.port, { confirmedProgressed: true });
      } else if (typeof PokemonNet.confirmLateJoin === "function") {
        PokemonNet.confirmLateJoin();
      } else if (ctx.port) {
        PokemonNet.joinRoom(ctx.port, { confirmedProgressed: true });
      }
      focusCanvas();
    });

    $("late-join-cancel-btn").addEventListener("click", () => {
      closeModal("late-join-modal");
    });

    $("peer-actions-strip").addEventListener("click", (e) => {
      const btn = e.target.closest("[data-action]");
      if (!btn || btn.disabled) return;
      const action = btn.dataset.action;
      const slotIdx = Number(btn.dataset.slot);
      if (action === "leave-party") {
        if (PokemonNet.state.runConfig && PokemonNet.state.runConfig.gameOver) return;
        PokemonNet.leaveParty();
        focusCanvas();
        return;
      }
      const gate = getUiFeatureGateReason(PokemonNet.state, Number.isInteger(slotIdx) ? slotIdx : null);
      if (gate) {
        showToast("warn", "Action Locked", gate);
        return;
      }
      if (action === "pair-party") PokemonNet.requestPairWith(slotIdx);
      else if (action === "teleport") PokemonNet.requestTeleport(slotIdx);
      else if (action === "pvp") PokemonNet.sendBattleInvite(slotIdx, "pvp");
      else if (action === "trade") {
        pendingTradeItems = [];
        PokemonNet.openTradeWith(slotIdx);
        openModal("trade-modal");
      }
      focusCanvas();
    });

    // Battle Join Toast & Cutscene Banner
    $("battle-accept-btn").addEventListener("click", () => {
      const inv = PokemonNet.state.activeInvites[0];
      if (inv) PokemonNet.respondBattleInvite(inv.inviteId, true);
      focusCanvas();
    });
    $("battle-decline-btn").addEventListener("click", () => {
      const inv = PokemonNet.state.activeInvites[0];
      if (inv) PokemonNet.respondBattleInvite(inv.inviteId, false);
      focusCanvas();
    });
    $("cutscene-finish-btn").addEventListener("click", () => {
      PokemonNet.finishActiveCutscene();
      focusCanvas();
    });

    // Quick-Trade Modal Controls
    $("trade-start-btn").addEventListener("click", () => {
      const slotVal = $("trade-partner-select").value;
      if (slotVal === "") {
        showToast("warn", "No Partner", "Connect another player to the room to Quick-Trade.");
        return;
      }
      const targetSlot = Number(slotVal);
      const gate = getUiFeatureGateReason(PokemonNet.state, targetSlot);
      if (gate) {
        showToast("warn", "Quick-Trade Locked", gate);
        return;
      }
      pendingTradeItems = [];
      PokemonNet.openTradeWith(targetSlot);
      renderTradeModal();
    });

    $("trade-partner-select").addEventListener("change", () => renderTradeModal());

    const syncMyTradeOffer = () => {
      const partyIdx = parseInt($("trade-my-mon-select").value, 10);
      const money = parseInt($("trade-my-money-input").value, 10) || 0;
      PokemonNet.updateMyTradeOffer(partyIdx, pendingTradeItems, money);
    };

    $("trade-my-mon-select").addEventListener("change", syncMyTradeOffer);
    $("trade-update-offer-btn").addEventListener("click", syncMyTradeOffer);

    $("trade-add-item-btn").addEventListener("click", () => {
      const me = PokemonNet.state.players[PokemonNet.state.connection.mySlotIndex];
      const itemId = $("trade-my-item-select").value;
      const qty = Math.max(1, parseInt($("trade-my-item-qty").value, 10) || 1);
      const inv = me && me.bagItems.find((i) => i.id === itemId);
      if (!inv) return;
      const existing = pendingTradeItems.find((i) => i.id === itemId);
      if (existing) existing.qty = Math.min(inv.qty, existing.qty + qty);
      else pendingTradeItems.push({ id: inv.id, name: inv.name, qty: Math.min(inv.qty, qty) });
      syncMyTradeOffer();
    });

    $("trade-my-items-list").addEventListener("click", (e) => {
      const btn = e.target.closest("[data-remove-trade-item]");
      if (!btn) return;
      pendingTradeItems.splice(Number(btn.dataset.removeTradeItem), 1);
      syncMyTradeOffer();
    });

    $("trade-lock-btn").addEventListener("click", () => {
      PokemonNet.toggleTradeLock(!PokemonNet.state.trade.myLocked);
    });
    $("trade-confirm-btn").addEventListener("click", () => {
      PokemonNet.confirmTrade();
    });
    $("trade-cancel-btn").addEventListener("click", () => {
      PokemonNet.cancelTrade();
    });

    // Run Save Manager Controls
    $("save-snapshot-now-btn").addEventListener("click", async () => {
      const snap = await PokemonNet.saveRunSnapshotToDb();
      showToast("ok", "Campaign Saved", `Saved ${snap.runId} to IndexedDB.`);
      renderSavesModal();
    });

    $("export-run-json-btn").addEventListener("click", () => {
      const snap = PokemonNet.buildRunSnapshot();
      downloadJson(snap, `pokemon_emerald_run_${snap.runId}.json`);
    });

    $("import-run-trigger-btn").addEventListener("click", () => {
      $("import-run-file").click();
    });

    if ($("reset-new-game-btn")) {
      $("reset-new-game-btn").addEventListener("click", () => {
        if ($("reset-new-game-confirm")) $("reset-new-game-confirm").style.display = "block";
      });
    }
    if ($("cancel-reset-btn")) {
      $("cancel-reset-btn").addEventListener("click", () => {
        if ($("reset-new-game-confirm")) $("reset-new-game-confirm").style.display = "none";
      });
    }
    if ($("confirm-reset-title-btn")) {
      $("confirm-reset-title-btn").addEventListener("click", () => {
        if ($("reset-new-game-confirm")) $("reset-new-game-confirm").style.display = "none";
        PokemonNet.resetNewGame({ mode: "title" });
        closeModal("saves-modal");
        showToast("warn", "New Game Reset", "Flash save erased — rebooted to Pokémon Emerald Title Screen.");
        focusCanvas();
      });
    }
    if ($("confirm-reset-quick-btn")) {
      $("confirm-reset-quick-btn").addEventListener("click", () => {
        if ($("reset-new-game-confirm")) $("reset-new-game-confirm").style.display = "none";
        PokemonNet.resetNewGame({ mode: "littleroot" });
        closeModal("saves-modal");
        showToast("warn", "Quick-Start New Game", "Flash save erased — started fresh in Littleroot Town.");
        focusCanvas();
      });
    }

    $("import-run-file").addEventListener("change", (ev) => {
      const file = ev.target.files && ev.target.files[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = async () => {
        try {
          const parsed = JSON.parse(String(reader.result || ""));
          if (!parsed || !parsed.runConfig) throw new Error("Missing runConfig in JSON");
          PokemonNet.applySnapshotLocal(parsed, PokemonNet.isHost());
          await PokemonNet.saveRunSnapshotToDb(parsed);
          showToast("ok", "Run Imported", `Loaded ${parsed.runConfig.roomName || parsed.runId}`);
          renderSavesModal();
        } catch (err) {
          showToast("danger", "Import Failed", err.message);
        }
      };
      reader.readAsText(file);
      ev.target.value = "";
    });

    $("saves-list-container").addEventListener("click", async (e) => {
      await handleSavedRunsAction(e, "saves-modal");
    });

    // Account Auth Modal & Controls
    let authModalMode = "login";
    const openAuthWithMode = (mode) => {
      authModalMode = mode === "register" ? "register" : "login";
      for (const t of document.querySelectorAll("[data-auth-tab]")) {
        t.classList.toggle("active", t.dataset.authTab === authModalMode);
      }
      const submitBtn = $("auth-submit-btn");
      if (submitBtn) submitBtn.textContent = authModalMode === "register" ? "Create Account" : "Sign In";
      const pwInput = $("auth-password-input");
      if (pwInput) pwInput.autocomplete = authModalMode === "register" ? "new-password" : "current-password";
      const errBox = $("auth-error-box");
      if (errBox) errBox.style.display = "none";
      openModal("auth-modal");
      if ($("auth-username-input")) $("auth-username-input").focus();
    };
    if ($("open-auth-btn")) $("open-auth-btn").addEventListener("click", () => openAuthWithMode("login"));
    if ($("saves-open-auth-btn")) $("saves-open-auth-btn").addEventListener("click", () => openAuthWithMode("login"));
    if ($("account-logout-btn")) {
      $("account-logout-btn").addEventListener("click", async () => {
        await PokemonNet.logoutAccount();
        showToast("info", "Signed Out", "Switched to Guest / Local mode.");
        focusCanvas();
      });
    }
    for (const tab of document.querySelectorAll("[data-auth-tab]")) {
      tab.addEventListener("click", () => {
        authModalMode = tab.dataset.authTab === "register" ? "register" : "login";
        for (const t of document.querySelectorAll("[data-auth-tab]")) {
          t.classList.toggle("active", t === tab);
        }
        const submitBtn = $("auth-submit-btn");
        if (submitBtn) submitBtn.textContent = authModalMode === "register" ? "Create Account" : "Sign In";
        const pwInput = $("auth-password-input");
        if (pwInput) pwInput.autocomplete = authModalMode === "register" ? "new-password" : "current-password";
        if ($("auth-error-box")) $("auth-error-box").style.display = "none";
      });
    }
    if ($("auth-form")) {
      $("auth-form").addEventListener("submit", async (e) => {
        e.preventDefault();
        const username = ($("auth-username-input") && $("auth-username-input").value.trim()) || "";
        const password = ($("auth-password-input") && $("auth-password-input").value) || "";
        const errBox = $("auth-error-box");
        const errText = $("auth-error-text");
        const submitBtn = $("auth-submit-btn");
        if (submitBtn) submitBtn.disabled = true;
        const res =
          authModalMode === "register"
            ? await PokemonNet.registerAccount(username, password)
            : await PokemonNet.loginAccount(username, password);
        if (submitBtn) submitBtn.disabled = false;
        if (!res || !res.ok) {
          if (errBox && errText) {
            errText.textContent = (res && res.error) || "Authentication failed";
            errBox.style.display = "block";
          }
          return;
        }
        if ($("auth-password-input")) $("auth-password-input").value = "";
        closeModal("auth-modal");
        showToast("ok", "Account Signed In", `Welcome, @${res.username}!`);
        focusCanvas();
      });
    }
    if ($("saves-refresh-cloud-btn")) {
      $("saves-refresh-cloud-btn").addEventListener("click", async () => {
        await PokemonNet.refreshCloudRuns();
        renderSavedRunsLists();
      });
    }
    if ($("rooms-refresh-saves-btn")) {
      $("rooms-refresh-saves-btn").addEventListener("click", async () => {
        await PokemonNet.refreshCloudRuns();
        renderSavedRunsLists();
      });
    }

    // Slot-Claim Modal Handlers
    if ($("slot-claim-list")) {
      $("slot-claim-list").addEventListener("click", async (e) => {
        const btn = e.target.closest("[data-claim-slot]");
        if (!btn) return;
        const slotIdx = Number(btn.dataset.claimSlot);
        $("slot-claim-modal").classList.remove("open");
        await PokemonNet.confirmSlotClaim(slotIdx);
        focusCanvas();
      });
    }
    if ($("slot-claim-new-btn")) {
      $("slot-claim-new-btn").addEventListener("click", () => {
        $("slot-claim-modal").classList.remove("open");
        PokemonNet.joinAsNewFromClaim();
        focusCanvas();
      });
    }
    if ($("slot-claim-cancel-btn")) {
      $("slot-claim-cancel-btn").addEventListener("click", () => {
        closeModal("slot-claim-modal");
      });
    }

    // Phase 0 Preview Sim Buttons (available until WASM boots)
    $("sim-catch-btn").addEventListener("click", () => {
      const pool = [
        { routeId: "ROUTE_103", routeName: "Route 103", species: "Wingull", nickname: "Peko", level: 7, hp: 23, maxHp: 23, ball: "pokeball" },
        { routeId: "ROUTE_104", routeName: "Route 104", species: "Taillow", nickname: "Sky", level: 8, hp: 25, maxHp: 25, ball: "greatball" },
        { routeId: "PETALBURG_WOODS", routeName: "Petalburg Woods", species: "Shroomish", nickname: "Cap", level: 9, hp: 29, maxHp: 29, ball: "ultraball" },
        { routeId: "RUSTURF_TUNNEL", routeName: "Rusturf Tunnel", species: "Kadabra", nickname: "Spoon", level: 16, hp: 38, maxHp: 38, ball: "greatball" },
      ];
      const pick = pool[Math.floor(Math.random() * pool.length)];
      PokemonNet.registerRouteCatch(PokemonNet.state.connection.mySlotIndex, pick.routeId, pick.routeName, pick);
    });

    $("sim-faint-btn").addEventListener("click", () => {
      const me = PokemonNet.state.players[PokemonNet.state.connection.mySlotIndex];
      if (!me || !me.party.length) {
        showToast("warn", "No Party Pokémon", "Catch a Pokémon first!");
        return;
      }
      PokemonNet.reportMonFainted(me.slotIndex, me.party[0].uid, "Crit by Gym Leader Nosepass");
    });

    $("sim-battle-btn").addEventListener("click", () => {
      const me = PokemonNet.state.players[PokemonNet.state.connection.mySlotIndex];
      PokemonNet.state.activeInvites.push({
        inviteId: "sim_" + Date.now(),
        mode: "coop_2v2",
        fromPeerId: me.peerId,
        fromName: "Brendan (Co-op)",
        mapName: "Route 110",
        enemyDesc: "Wild Lv.14 Electrike & Minun",
        battleSeed: 133742,
      });
      renderBattleInviteToast(PokemonNet.state);
    });

    $("sim-cutscene-btn").addEventListener("click", () => {
      const id = "EVT_PREVIEW_" + Math.floor(Math.random() * 1000);
      PokemonNet.triggerGlobalCutscene(id, "Mt. Chimney Team Aqua/Magma Showdown");
    });

    // Global Hotkeys (F2 = Hide/Show HUD, F11 = Fullscreen)
    global.addEventListener("keydown", (e) => {
      if (e.key === "F2") {
        e.preventDefault();
        toggleHud();
      } else if (e.key === "F11") {
        e.preventDefault();
        toggleFullscreen();
      }
    });

    const unlockAudioOnGesture = () => {
      if (!audioMuted) syncAudioContextMuteState();
    };
    for (const evName of ["pointerdown", "keydown", "touchstart", "click"]) {
      global.addEventListener(evName, unlockAudioOnGesture, { capture: true, passive: true });
    }

    // Track local keyboard GBA bits only as fallback when older WASM lacks _wasm_set_gamepad_mask
    const syncKeyTracker = (bit, down) => {
      if (!bit) return;
      const next = down ? trackedKeyboardMask | bit : trackedKeyboardMask & ~bit;
      if (next !== trackedKeyboardMask) {
        trackedKeyboardMask = next;
        if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge.setKeyboardMask === "function") {
          global.PokemonWasmBridge.setKeyboardMask(trackedKeyboardMask);
        }
      }
    };
    global.addEventListener(
      "keydown",
      (e) => {
        const ae = document.activeElement;
        if (ae && (ae.tagName === "INPUT" || ae.tagName === "TEXTAREA" || ae.tagName === "SELECT")) return;
        syncKeyTracker(DOM_CODE_TO_GBA_BIT[e.code] || 0, true);
      },
      { capture: true, passive: true }
    );
    global.addEventListener(
      "keyup",
      (e) => {
        syncKeyTracker(DOM_CODE_TO_GBA_BIT[e.code] || 0, false);
      },
      { capture: true, passive: true }
    );
    global.addEventListener(
      "blur",
      () => {
        trackedKeyboardMask = 0;
        if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge.setKeyboardMask === "function") {
          global.PokemonWasmBridge.setKeyboardMask(0);
        }
        if (lastGamepadMask !== 0) {
          lastGamepadMask = 0;
          if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge.setInputMask === "function") {
            global.PokemonWasmBridge.setInputMask(0, "gamepad");
          }
        }
      },
      { passive: true }
    );

    global.addEventListener("gamepadconnected", () => {
      startGamepadPolling();
      pollGamepadsOnce();
    });
    global.addEventListener("gamepaddisconnected", () => {
      pollGamepadsOnce();
    });

    document.addEventListener("fullscreenchange", syncFullscreenEscapeLock);
  }

  function readBrowserGamepadMask() {
    if (typeof navigator === "undefined" || typeof navigator.getGamepads !== "function") return 0;
    let pads;
    try {
      pads = navigator.getGamepads();
    } catch (_) {
      return 0;
    }
    if (!pads) return 0;
    const deadzone = 0.35;
    let mask = 0;
    for (let p = 0; p < pads.length; p++) {
      const gp = pads[p];
      if (!gp || gp.connected === false) continue;
      const btns = gp.buttons || [];
      const axes = gp.axes || [];
      const pressed = (idx) => {
        const b = btns[idx];
        return Boolean(b && (b.pressed || (typeof b.value === "number" && b.value > 0.5)));
      };
      const axis = (idx) => (typeof axes[idx] === "number" ? axes[idx] : 0);

      if (pressed(0)) mask |= 1 << 0; // A (South / Xbox A / Cross)
      if (pressed(1) || pressed(2)) mask |= 1 << 1; // B (East / West / Circle / Square)
      if (pressed(8)) mask |= 1 << 2; // SELECT (Back / View / Share)
      if (pressed(9)) mask |= 1 << 3; // START (Start / Menu / Options)
      if (pressed(15) || axis(0) > deadzone) mask |= 1 << 4; // RIGHT
      if (pressed(14) || axis(0) < -deadzone) mask |= 1 << 5; // LEFT
      if (pressed(12) || axis(1) < -deadzone) mask |= 1 << 6; // UP
      if (pressed(13) || axis(1) > deadzone) mask |= 1 << 7; // DOWN
      if (pressed(5) || pressed(7)) mask |= 1 << 8; // R (RB / RT)
      if (pressed(4) || pressed(6)) mask |= 1 << 9; // L (LB / LT)
    }
    return mask & 0x03ff;
  }

  function pollGamepadsOnce() {
    const ae = document.activeElement;
    const typingInForm =
      ae && (ae.tagName === "INPUT" || ae.tagName === "TEXTAREA" || ae.tagName === "SELECT");
    const mask = typingInForm ? 0 : readBrowserGamepadMask();
    if (mask !== lastGamepadMask) {
      lastGamepadMask = mask;
      if (global.PokemonWasmBridge && typeof global.PokemonWasmBridge.setInputMask === "function") {
        global.PokemonWasmBridge.setInputMask(mask, "gamepad");
      }
      if (mask !== 0 && !audioMuted) {
        syncAudioContextMuteState();
      }
    }
  }

  function startGamepadPolling() {
    if (gamepadPollStarted) return;
    if (typeof navigator === "undefined" || typeof navigator.getGamepads !== "function") return;
    gamepadPollStarted = true;
    const tick = () => {
      pollGamepadsOnce();
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  }

  // --- Boot Entry ---
  document.addEventListener("DOMContentLoaded", () => {
    applyAudioMuteState();
    bindEvents();
    PokemonNet.subscribe(renderAll);
    renderAll(PokemonNet.state, "init");
    PokemonNet.validateSession().catch(() => {});
    PokemonNet.refreshRooms();
    setInterval(() => PokemonNet.refreshRooms(), 3500);
    startGamepadPolling();
    bootGameOrPreview();
  });
})(typeof window !== "undefined" ? window : globalThis);
