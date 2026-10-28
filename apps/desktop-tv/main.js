(function () {
    'use strict';
    var stage = document.getElementById('stage');
    var desktop = document.getElementById('desktop');
    var setup = document.getElementById('setup');
    var status = document.getElementById('status');
    var endpoint = document.getElementById('endpoint');
    var secret = document.getElementById('secret');
    var connectButton = document.getElementById('connect');
    var forgetButton = document.getElementById('forget');
    var fitButton = document.getElementById('fit-mode');
    var banner = document.getElementById('banner');
    var controls = [endpoint, secret, connectButton, forgetButton, fitButton, document.getElementById('leave')];
    var pairingKey = 'omarchy.display.pairing.v1';
    var savedPair = null;
    var pairingRemembered = false;
    var storageError = false;
    var socket = null;
    var timeout = null;
    var bannerTimer = null;
    var metadataRequest = null;
    var dimensionsTimer = null;
    var phase = 'ready';
    var fitMode = 'contain';
    var frameWidth = 0;
    var frameHeight = 0;
    var sourceFPS = 0;
    var decodedFrames = 0;
    var decodeMilliseconds = 0;
    var connectedAt = 0;
    var decoding = false;
    var front = document.createElement('img');
    var back = document.createElement('img');
    front.className = back.className = 'desktop-frame';
    front.alt = back.alt = '';
    front.style.visibility = back.style.visibility = 'hidden';
    desktop.appendChild(front);
    desktop.appendChild(back);

    function message(text) { status.textContent = text; }
    function fit() {
        var scale = Math.min(window.innerWidth / 1920, window.innerHeight / 1080);
        stage.style.transform = 'scale(' + scale + ')';
        stage.style.left = Math.round((window.innerWidth - 1920 * scale) / 2) + 'px';
        stage.style.top = Math.round((window.innerHeight - 1080 * scale) / 2) + 'px';
        front.style.objectFit = back.style.objectFit = fitMode;
        fitButton.textContent = fitMode === 'contain' ? 'Fit: Screen' : 'Fit: App pixels';
    }
    function clearImage(image) {
        image.onload = image.onerror = null;
        image.removeAttribute('src');
        image.style.visibility = 'hidden';
        if (image.frameURL) { URL.revokeObjectURL(image.frameURL); image.frameURL = null; }
    }
    function disconnect(text) {
        var old = socket;
        socket = null;
        phase = 'disconnected';
        window.clearTimeout(timeout);
        window.clearTimeout(bannerTimer);
        window.clearTimeout(dimensionsTimer);
        if (metadataRequest) { metadataRequest.abort(); metadataRequest = null; }
        clearImage(front);
        clearImage(back);
        decoding = false;
        banner.className = 'hidden';
        setup.className = '';
        connectButton.disabled = false;
        connectButton.textContent = 'Connect';
        if (old) { old.onopen = old.onmessage = old.onerror = old.onclose = null; old.close(); }
        message(text || 'Disconnected. The TV workspace stays available until you stop sharing locally.');
        connectButton.focus();
    }
    function exit() {
        disconnect('Disconnected.');
        try {
            if (window.tizen && tizen.application) { tizen.application.getCurrentApplication().exit(); return; }
        } catch (error) { message('Could not exit the TV application.'); return; }
        message('Disconnected. You can close this browser tab.');
    }
    function reportDimensions(parsed, pairing) {
        if (metadataRequest) { metadataRequest.abort(); metadataRequest = null; }
        var request = new XMLHttpRequest();
        metadataRequest = request;
        request.open('POST', (parsed.protocol === 'wss:' ? 'https://' : 'http://') + parsed.host + '/receiver-info', true);
        request.timeout = 5000;
        request.setRequestHeader('Content-Type', 'application/json');
        request.setRequestHeader('Authorization', 'Bearer ' + pairing);
        request.onload = function () {
            if (metadataRequest !== request) { return; }
            metadataRequest = null;
            if (request.status !== 204 && request.status !== 200) { message('Connected; the host did not accept TV viewport information.'); }
        };
        request.onerror = request.ontimeout = function () {
            if (metadataRequest === request) { metadataRequest = null; message('Connected; TV viewport information could not reach the host.'); }
        };
        // Samsung's screen/SystemInfo DISPLAY values can describe app resolution,
        // not the physical UHD panel. Never relabel them as a native-panel size.
        request.send(JSON.stringify({viewportWidth: window.innerWidth, viewportHeight: window.innerHeight, panelWidth: null, panelHeight: null}));
    }
    function remember(url, pairing) {
        savedPair = {endpoint: url, secret: pairing};
        pairingRemembered = false;
        try { window.localStorage.setItem(pairingKey, JSON.stringify(savedPair)); pairingRemembered = true; }
        catch (error) { storageError = true; }
        forgetButton.disabled = false;
        secret.value = '';
        banner.textContent = pairingRemembered ? 'Connected · Back to disconnect · View only' :
            'Connected for this session only · TV storage could not remember pairing';
        banner.className = '';
        if (pairingRemembered) { bannerTimer = window.setTimeout(function () { banner.className = 'hidden'; }, 5000); }
        message(pairingRemembered ? 'Connected. Successful pairing is remembered on this TV.' :
            'Connected, but pairing could not be saved. Check the TV app storage.');
    }
    function dimension(value) { return typeof value === 'number' && value % 1 === 0 && value >= 320 && value <= 8192; }
    function connect(event) {
        if (event) { event.preventDefault(); }
        if (socket) { return; }
        var url = endpoint.value.replace(/^\s+|\s+$/g, '');
        var pairing = secret.value.replace(/^\s+|\s+$/g, '').toLowerCase();
        if (!pairing && savedPair && savedPair.endpoint === url) { pairing = savedPair.secret; }
        var parsed;
        try {
            parsed = new URL(url);
            if ((parsed.protocol !== 'ws:' && parsed.protocol !== 'wss:') || parsed.pathname !== '/desktop' ||
                    parsed.username || parsed.password || parsed.search || parsed.hash) { throw new Error('endpoint'); }
        } catch (error) { message('Use ws://host:port/desktop or wss://host:port/desktop without credentials or query.'); endpoint.focus(); return; }
        if (!/^[0-9a-f]{32}$/.test(pairing)) { message('Enter the full 32-character host pairing secret.'); secret.focus(); return; }
        endpoint.blur();
        secret.blur();
        var current;
        try { current = new WebSocket(url, ['binary', 'pair.' + pairing]); }
        catch (error) { message('The desktop WebSocket could not be opened.'); return; }
        socket = current;
        current.binaryType = 'arraybuffer';
        phase = 'connecting';
        frameWidth = frameHeight = sourceFPS = decodedFrames = decodeMilliseconds = connectedAt = 0;
        connectButton.disabled = true;
        connectButton.textContent = 'Connecting…';
        message('Waiting for the first real desktop frame…');
        var hello = false;
        current.onmessage = function (event) {
            if (socket !== current) { return; }
            if (typeof event.data === 'string') {
                try {
                    var info = JSON.parse(event.data);
                    if (hello || info.type !== 'hello' || !dimension(info.width) || !dimension(info.height) ||
                            [15, 30, 60].indexOf(info.fps) < 0 || info.viewOnly !== true || info.audio !== false) { throw new Error('hello'); }
                    frameWidth = info.width;
                    frameHeight = info.height;
                    sourceFPS = info.fps;
                    hello = true;
                } catch (error) { disconnect('The host returned an incompatible desktop stream.'); }
                return;
            }
            var data = event.data;
            if (!hello || !(data instanceof ArrayBuffer) || data.byteLength <= 20 || data.byteLength > 16 * 1024 * 1024 || decoding) {
                disconnect('The host violated the bounded-frame streaming protocol.'); return;
            }
            var header = new DataView(data);
            if (header.getUint32(0) !== 0x4f444a31 || header.getUint16(16) !== frameWidth || header.getUint16(18) !== frameHeight) {
                disconnect('The desktop frame dimensions or protocol changed unexpectedly.'); return;
            }
            var sequence = header.getUint32(4);
            var target = back;
            var began = performance.now();
            decoding = true;
            try { target.frameURL = URL.createObjectURL(new Blob([new Uint8Array(data, 20)], {type: 'image/jpeg'})); }
            catch (error) { disconnect('This TV could not allocate a desktop image.'); return; }
            target.onload = function () {
                if (socket !== current) { return; }
                if (target.naturalWidth !== frameWidth || target.naturalHeight !== frameHeight) { disconnect('Decoded desktop dimensions did not match the host.'); return; }
                front.style.visibility = 'hidden';
                target.style.visibility = 'visible';
                front.onload = front.onerror = null;
                if (front.frameURL) { URL.revokeObjectURL(front.frameURL); front.frameURL = null; }
                var old = front;
                front = target;
                back = old;
                decodeMilliseconds += performance.now() - began;
                decodedFrames++;
                decoding = false;
                var firstFrame = phase !== 'connected';
                phase = 'connected';
                if (firstFrame) {
                    window.clearTimeout(timeout);
                    connectedAt = Date.now();
                    setup.className = 'hidden';
                    remember(url, pairing);
                }
                try { current.send(JSON.stringify({type: 'ack', sequence: sequence})); }
                catch (error) { disconnect('The desktop connection ended while acknowledging its frame.'); return; }
                if (firstFrame) { reportDimensions(parsed, pairing); }
            };
            target.onerror = function () { if (socket === current) { disconnect('The TV could not decode a desktop image.'); } };
            target.src = target.frameURL;
        };
        current.onerror = function () { if (socket === current) { disconnect('Connection failed. Check the host, endpoint, pairing and TV-only firewall rule.'); } };
        current.onclose = function () { if (socket === current) { disconnect('Desktop connection closed. Start sharing locally, then Connect to reuse saved pairing.'); } };
        timeout = window.setTimeout(function () { if (socket === current) { disconnect('No desktop frame arrived. Check the host capture, pairing and local network.'); } }, 15000);
    }
    function forgetPairing() {
        disconnect('Clearing saved pairing…');
        try { window.localStorage.removeItem(pairingKey); }
        catch (error) { message('Could not clear TV storage. Rotate the host secret to revoke this TV.'); return; }
        savedPair = null;
        pairingRemembered = false;
        secret.value = '';
        forgetButton.disabled = true;
        message('Pairing forgotten. Exit normally to let TV storage finish writing.');
        secret.focus();
    }
    document.getElementById('connection').addEventListener('submit', connect);
    document.getElementById('leave').addEventListener('click', exit);
    forgetButton.addEventListener('click', forgetPairing);
    fitButton.addEventListener('click', function () {
        fitMode = fitMode === 'contain' ? 'none' : 'contain';
        fit();
        try { window.localStorage.setItem('omarchy.display.fit', fitMode); }
        catch (error) { message('Sizing changed for this session only; TV storage is unavailable.'); }
    });
    document.addEventListener('keydown', function (event) {
        var code = event.keyCode;
        if (code === 10009 || code === 27) { event.preventDefault(); if (socket) { disconnect(); } else { exit(); } return; }
        if (setup.className === 'hidden' || (code !== 38 && code !== 40)) { return; }
        var direction = code === 38 ? -1 : 1;
        var next = (controls.indexOf(document.activeElement) + direction + controls.length) % controls.length;
        var skipped = 0;
        while (controls[next].disabled && skipped++ < controls.length) { next = (next + direction + controls.length) % controls.length; }
        event.preventDefault();
        controls[next].focus();
    });
    document.addEventListener('visibilitychange', function () {
        if (document.hidden && socket) { disconnect('Disconnected while the app is in the background.'); }
        else if (!document.hidden && savedPair && !socket) { connect(); }
    });
    window.addEventListener('resize', function () {
        fit();
        window.clearTimeout(dimensionsTimer);
        if (socket && phase === 'connected' && savedPair) {
            dimensionsTimer = window.setTimeout(function () {
                if (socket && phase === 'connected' && savedPair) { reportDimensions(new URL(savedPair.endpoint), savedPair.secret); }
            }, 250);
        }
    });
    window.addEventListener('beforeunload', function () { if (socket) { socket.close(); } });
    try {
        var stored = JSON.parse(window.localStorage.getItem(pairingKey));
        if (stored && typeof stored.endpoint === 'string' && /^[0-9a-f]{32}$/.test(stored.secret)) {
            savedPair = {endpoint: stored.endpoint, secret: stored.secret};
            pairingRemembered = true;
            endpoint.value = stored.endpoint;
            forgetButton.disabled = false;
        } else { endpoint.value = window.localStorage.getItem('omarchy.display.endpoint') || ''; }
        if (window.localStorage.getItem('omarchy.display.fit') === 'none') { fitMode = 'none'; }
    } catch (error) { storageError = true; }
    window.OmarchyDisplayQA = {status: function () {
        return {phase: phase, clientReady: true, connected: phase === 'connected', viewOnly: true, audio: false,
            pairingRemembered: pairingRemembered, frameWidth: frameWidth, frameHeight: frameHeight,
            configuredFPS: sourceFPS, decodedFrames: decodedFrames, decodeMillisecondsTotal: decodeMilliseconds,
            sessionSeconds: connectedAt ? (Date.now() - connectedAt) / 1000 : 0, fitMode: fitMode};
    }};
    connectButton.disabled = false;
    fit();
    if (!savedPair) { endpoint.focus(); }
    message(storageError ? 'TV storage could not be read. Pairing may work for this session only.' :
        'Pair once. TV app viewport: ' + window.innerWidth + ' × ' + window.innerHeight + '; physical panel resolution is not inferred.');
    if (savedPair && !document.hidden) { connect(); }
}());
