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
    var connectedAt = 0;
    var gl = null;
    var texture = null;
    var fillTarget = null;
    var fillProgram = null;
    var fillPosition = null;
    var fillColour = null;
    var program = null;
    var vertices = null;
    var position = null;
    var atlasTexture = null;
    var atlasCapacityWidth = 0;
    var atlasCapacityHeight = 0;
    var atlasProgram = null;
    var atlasPosition = null;
    var atlasMapping = null;
    var maxTextureSize = null;
    var maxViewport = null;
    var batch = null;
    var hasBaseline = false;
    var presentedFrames = 0;
    var decodedRegions = 0;
    var decodedAtlasImages = 0;
    var filledRegions = 0;
    var lastSequence = null;
    var lastCapturePtsUs = null;
    var lastEncodedWallMs = null;
    var lastPresentedAt = 0;
    var rendererError = null;
    var proofRequest = null;
    var pixelProof = null;
    var proofId = 0;
    var blobURLs = window.URL || window.webkitURL;

    function message(text) { status.textContent = text; }
    function integer(value, minimum, maximum) {
        return typeof value === 'number' && isFinite(value) && value % 1 === 0 && value >= minimum && value <= maximum;
    }
    function fit() {
        var scale = Math.min(window.innerWidth / 1920, window.innerHeight / 1080);
        stage.style.transform = 'scale(' + scale + ')';
        stage.style.left = Math.round((window.innerWidth - 1920 * scale) / 2) + 'px';
        stage.style.top = Math.round((window.innerHeight - 1080 * scale) / 2) + 'px';
        fitButton.textContent = fitMode === 'contain' ? 'Fit: Screen' : 'Fit: App pixels';
        if (!frameWidth || !scale) { return; }
        var factor = fitMode === 'contain' ? Math.min(1920 / frameWidth, 1080 / frameHeight) : 1 / scale;
        var width = frameWidth * factor;
        var height = frameHeight * factor;
        desktop.style.width = width + 'px';
        desktop.style.height = height + 'px';
        desktop.style.left = ((1920 - width) / 2) + 'px';
        desktop.style.top = ((1080 - height) / 2) + 'px';
    }
    function releaseRegion(region) {
        if (region.image) {
            region.image.onload = region.image.onerror = null;
            region.image.src = '';
            region.image = null;
        }
        if (region.url) { blobURLs.revokeObjectURL(region.url); region.url = null; }
        region.jpeg = null;
    }
    function releaseBatch() {
        if (!batch) { return; }
        var old = batch;
        batch = null;
        for (var i = 0; i < old.regions.length; i++) { releaseRegion(old.regions[i]); }
    }
    function closeGPU() {
        releaseBatch();
        proofRequest = null;
        hasBaseline = false;
        if (gl) {
            gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            gl.deleteFramebuffer(fillTarget);
            gl.deleteTexture(texture);
            gl.deleteTexture(atlasTexture);
            gl.deleteProgram(atlasProgram);
            gl.deleteProgram(fillProgram);
            gl.deleteBuffer(vertices);
            gl.deleteProgram(program);
            gl.bindTexture(gl.TEXTURE_2D, null);
            gl.bindBuffer(gl.ARRAY_BUFFER, null);
            gl.useProgram(null);
        }
        texture = vertices = program = null;
        fillTarget = null;
        fillProgram = fillPosition = fillColour = null;
        position = atlasTexture = atlasProgram = atlasPosition = atlasMapping = null;
        atlasCapacityWidth = atlasCapacityHeight = 0;
        // A 1x1 default framebuffer releases the large drawing buffer as well.
        desktop.width = desktop.height = 1;
        desktop.className = 'hidden';
    }
    function disconnect(text) {
        var old = socket;
        socket = null;
        phase = 'disconnected';
        window.clearTimeout(timeout);
        window.clearTimeout(bannerTimer);
        window.clearTimeout(dimensionsTimer);
        if (metadataRequest) { metadataRequest.abort(); metadataRequest = null; }
        closeGPU();
        banner.className = 'hidden';
        setup.className = '';
        connectButton.disabled = false;
        connectButton.textContent = 'Connect';
        if (old) {
            old.onopen = old.onmessage = old.onerror = old.onclose = null;
            try { old.close(); } catch (error) { /* Already closed. */ }
        }
        message(text || 'Disconnected. The TV workspace stays available until you stop sharing locally.');
        connectButton.focus();
    }
    function fail(code, text) {
        rendererError = code;
        // Neither server messages nor exception text are displayed: they may contain credentials.
        disconnect(text || 'The desktop image could not be presented. Reconnect after checking the host.');
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
        // App graphics dimensions are not evidence of physical panel resolution.
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
    function armTimeout(current, milliseconds, text) {
        window.clearTimeout(timeout);
        timeout = window.setTimeout(function () { if (socket === current) { disconnect(text); } }, milliseconds);
    }
    function gpuOK() {
        return gl && !gl.isContextLost() && gl.getError() === gl.NO_ERROR;
    }
    function shader(type, source) {
        var result = gl.createShader(type);
        if (!result) { throw new Error('shader'); }
        gl.shaderSource(result, source);
        gl.compileShader(result);
        if (!gl.getShaderParameter(result, gl.COMPILE_STATUS)) { gl.deleteShader(result); throw new Error('shader'); }
        return result;
    }
    function openGPU() {
        var vertex = null;
        var fragment = null;
        try {
            if (!gl) {
                gl = desktop.getContext('webgl', {alpha: false, antialias: false, depth: false, stencil: false, preserveDrawingBuffer: false}) ||
                    desktop.getContext('experimental-webgl', {alpha: false, antialias: false, depth: false, stencil: false, preserveDrawingBuffer: false});
            }
            if (!gl || gl.isContextLost()) { throw new Error('context'); }
            maxTextureSize = gl.getParameter(gl.MAX_TEXTURE_SIZE);
            maxViewport = gl.getParameter(gl.MAX_VIEWPORT_DIMS);
            if (frameWidth > maxTextureSize || frameHeight > maxTextureSize ||
                    frameWidth > maxViewport[0] || frameHeight > maxViewport[1]) { throw new Error('size'); }
            // Do not silently reduce capture or the receiver backing resolution.
            desktop.width = frameWidth;
            desktop.height = frameHeight;
            if (gl.drawingBufferWidth !== frameWidth || gl.drawingBufferHeight !== frameHeight) { throw new Error('backing'); }
            vertex = shader(gl.VERTEX_SHADER, 'attribute vec2 position; varying highp vec2 uv; void main(){ gl_Position=vec4(position,0.0,1.0); uv=vec2((position.x+1.0)*0.5,(1.0-position.y)*0.5); }');
            fragment = shader(gl.FRAGMENT_SHADER, 'precision highp float; varying highp vec2 uv; uniform sampler2D image; void main(){ gl_FragColor=texture2D(image,uv); }');
            program = gl.createProgram();
            gl.attachShader(program, vertex);
            gl.attachShader(program, fragment);
            gl.linkProgram(program);
            if (!gl.getProgramParameter(program, gl.LINK_STATUS)) { throw new Error('link'); }
            gl.deleteShader(vertex); vertex = null;
            gl.deleteShader(fragment); fragment = null;
            gl.useProgram(program);
            vertices = gl.createBuffer();
            gl.bindBuffer(gl.ARRAY_BUFFER, vertices);
            gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
            position = gl.getAttribLocation(program, 'position');
            gl.enableVertexAttribArray(position);
            gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 0);
            texture = gl.createTexture();
            gl.activeTexture(gl.TEXTURE0);
            gl.bindTexture(gl.TEXTURE_2D, texture);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
            gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
            gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
            gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
            gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.NONE);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, frameWidth, frameHeight, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
            // Attach the existing source texture: fills need no second 4K colour buffer.
            fillTarget = gl.createFramebuffer();
            gl.bindFramebuffer(gl.FRAMEBUFFER, fillTarget);
            gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
            if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) { throw new Error('fill-target'); }
            gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            gl.uniform1i(gl.getUniformLocation(program, 'image'), 0);
            gl.disable(gl.BLEND);
            gl.disable(gl.DEPTH_TEST);
            gl.disable(gl.SCISSOR_TEST);
            gl.disable(gl.DITHER);
            gl.viewport(0, 0, frameWidth, frameHeight);
            if (!gpuOK()) { throw new Error('gpu'); }
            fit();
            return true;
        } catch (error) {
            if (vertex) { gl.deleteShader(vertex); }
            if (fragment) { gl.deleteShader(fragment); }
            fail('WEBGL_INITIALIZATION', 'This TV could not allocate the full source WebGL surface. Capture resolution was not reduced.');
            return false;
        }
    }
    function openFillGPU() {
        if (fillProgram) { return; }
        var vertex = null;
        var fragment = null;
        try {
            vertex = shader(gl.VERTEX_SHADER, 'attribute vec2 position; void main(){ gl_Position=vec4(position,0.0,1.0); }');
            fragment = shader(gl.FRAGMENT_SHADER, 'precision highp float; uniform vec4 colour; void main(){ gl_FragColor=colour; }');
            fillProgram = gl.createProgram();
            gl.attachShader(fillProgram, vertex);
            gl.attachShader(fillProgram, fragment);
            gl.linkProgram(fillProgram);
            if (!gl.getProgramParameter(fillProgram, gl.LINK_STATUS)) { throw new Error('fill-link'); }
            fillPosition = gl.getAttribLocation(fillProgram, 'position');
            fillColour = gl.getUniformLocation(fillProgram, 'colour');
        } finally {
            if (vertex) { gl.deleteShader(vertex); }
            if (fragment) { gl.deleteShader(fragment); }
        }
    }
    function openAtlasGPU() {
        if (atlasTexture) { return; }
        var vertex = null;
        var fragment = null;
        try {
            // The FBO's y=0 and DOM upload's first row both mean source top.
            // Unlike the final presentation shader, this quad must not invert y.
            vertex = shader(gl.VERTEX_SHADER, 'attribute vec2 position; varying highp vec2 uv; uniform vec4 mapping; void main(){ gl_Position=vec4(position,0.0,1.0); uv=mapping.xy+(position+1.0)*0.5*mapping.zw; }');
            fragment = shader(gl.FRAGMENT_SHADER, 'precision highp float; varying highp vec2 uv; uniform sampler2D image; void main(){ gl_FragColor=texture2D(image,uv); }');
            atlasProgram = gl.createProgram();
            gl.attachShader(atlasProgram, vertex);
            gl.attachShader(atlasProgram, fragment);
            gl.linkProgram(atlasProgram);
            if (!gl.getProgramParameter(atlasProgram, gl.LINK_STATUS)) { throw new Error('atlas-link'); }
            atlasPosition = gl.getAttribLocation(atlasProgram, 'position');
            atlasMapping = gl.getUniformLocation(atlasProgram, 'mapping');
            gl.useProgram(atlasProgram);
            gl.uniform1i(gl.getUniformLocation(atlasProgram, 'image'), 0);
            atlasTexture = gl.createTexture();
            gl.bindTexture(gl.TEXTURE_2D, atlasTexture);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
            if (!gpuOK()) { throw new Error('atlas-allocation'); }
        } finally {
            if (vertex) { gl.deleteShader(vertex); }
            if (fragment) { gl.deleteShader(fragment); }
        }
    }
    function useQuad(drawProgram, attribute) {
        gl.useProgram(drawProgram);
        gl.bindBuffer(gl.ARRAY_BUFFER, vertices);
        gl.enableVertexAttribArray(attribute);
        gl.vertexAttribPointer(attribute, 2, gl.FLOAT, false, 0, 0);
    }
    function copyAtlas(region) {
        openAtlasGPU();
        // Staging is the only sampled texture; the authoritative source is the
        // destination FBO, never a sampler in this draw (no feedback loop).
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.bindTexture(gl.TEXTURE_2D, atlasTexture);
        if (region.imageWidth > atlasCapacityWidth || region.imageHeight > atlasCapacityHeight) {
            // Retain capacity across atlases and batches; only increasing extents
            // replace storage. Sparse batches never preallocate a full 4K atlas.
            var width = Math.max(atlasCapacityWidth, region.imageWidth);
            var height = Math.max(atlasCapacityHeight, region.imageHeight);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
            if (!gpuOK()) { throw new Error('atlas-growth'); }
            atlasCapacityWidth = width;
            atlasCapacityHeight = height;
        }
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, region.image);
        gl.bindFramebuffer(gl.FRAMEBUFFER, fillTarget);
        gl.disable(gl.SCISSOR_TEST);
        gl.disable(gl.BLEND);
        useQuad(atlasProgram, atlasPosition);
        for (var i = 0; i < region.maps.length; i++) {
            var map = region.maps[i];
            gl.viewport(map.x, map.y, map.width, map.height);
            // Raster pixel centres interpolate to exact atlas texel centres.
            gl.uniform4f(atlasMapping, map.atlasX / atlasCapacityWidth, map.atlasY / atlasCapacityHeight,
                map.width / atlasCapacityWidth, map.height / atlasCapacityHeight);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
    }
    function jpegDimensions(bytes, width, height) {
        if (bytes.length < 4 || bytes[0] !== 255 || bytes[1] !== 216 ||
                bytes[bytes.length - 2] !== 255 || bytes[bytes.length - 1] !== 217) { return false; }
        var offset = 2;
        while (offset < bytes.length - 2) {
            if (bytes[offset++] !== 255) { return false; }
            while (offset < bytes.length && bytes[offset] === 255) { offset++; }
            if (offset >= bytes.length) { return false; }
            var marker = bytes[offset++];
            if (marker === 218 || marker === 217 || marker === 0 || marker === 216) { return false; }
            if (marker === 1 || (marker >= 208 && marker <= 215)) { continue; }
            if (offset + 2 > bytes.length) { return false; }
            var length = bytes[offset] * 256 + bytes[offset + 1];
            if (length < 2 || offset + length > bytes.length) { return false; }
            if (marker === 192 || marker === 193 || marker === 194) {
                return length >= 8 && bytes[offset + 2] === 8 &&
                    bytes[offset + 3] * 256 + bytes[offset + 4] === height &&
                    bytes[offset + 5] * 256 + bytes[offset + 6] === width &&
                    (bytes[offset + 7] === 1 || bytes[offset + 7] === 3) && length === 8 + 3 * bytes[offset + 7];
            }
            offset += length;
        }
        return false;
    }
    function boxesOverlap(a, b) {
        return a.x < b.x + b.width && b.x < a.x + a.width &&
            a.y < b.y + b.height && b.y < a.y + a.height;
    }
    function addSourceBox(sources, box) {
        if (!box.width || !box.height || box.x + box.width > frameWidth || box.y + box.height > frameHeight ||
                sources.length >= 64) { throw new Error('source-bounds'); }
        for (var i = 0; i < sources.length; i++) {
            if (boxesOverlap(box, sources[i])) { throw new Error('source-overlap'); }
        }
        sources.push(box);
        return box.width * box.height;
    }
    function parseBatch(data) {
        if (!(data instanceof ArrayBuffer) || data.byteLength < 52 || data.byteLength > 32 * 1024 * 1024) { throw new Error('size'); }
        var view = new DataView(data);
        if (view.getUint32(0, false) !== 0x4f445233) { throw new Error('magic'); }
        var count = view.getUint16(28, false);
        var flags = view.getUint16(30, false);
        var capture = view.getFloat64(8, false);
        var wall = view.getFloat64(16, false);
        if (view.getUint16(24, false) !== frameWidth || view.getUint16(26, false) !== frameHeight ||
                count < 1 || count > 64 || flags > 1 || !isFinite(capture) || capture < 0 ||
                !isFinite(wall) || wall < 0 || 32 + count * 16 > data.byteLength || (!hasBaseline && flags !== 1)) { throw new Error('header'); }
        var result = {sequence: view.getUint32(4, false), capturePtsUs: capture, encodedWallMs: wall,
            full: flags === 1, regions: [], next: 0, active: 0, completed: 0};
        if (lastSequence !== null && result.sequence === lastSequence) { throw new Error('sequence'); }
        var offset = 32 + count * 16;
        var area = 0;
        var decodeArea = 0;
        var sources = [];
        for (var i = 0; i < count; i++) {
            var entry = 32 + i * 16;
            var region = {x: view.getUint16(entry, false), y: view.getUint16(entry + 2, false),
                width: view.getUint16(entry + 4, false), height: view.getUint16(entry + 6, false),
                kind: view.getUint32(entry + 8, false), image: null, url: null};
            var length = view.getUint32(entry + 12, false);
            if (!region.width || !region.height || region.x + region.width > frameWidth || region.y + region.height > frameHeight ||
                    (region.kind !== 0 && region.kind !== 1 && region.kind !== 2) || length < (region.kind === 2 ? 24 : 4) ||
                    (region.kind === 1 && length !== 4) || length > data.byteLength - offset) { throw new Error('rectangle'); }
            var payload = new Uint8Array(data, offset, length);
            if (region.kind === 2) {
                region.imageWidth = view.getUint16(offset, false);
                region.imageHeight = view.getUint16(offset + 2, false);
                var mappingCount = view.getUint16(offset + 4, false);
                var header = 8 + mappingCount * 12;
                if (!region.imageWidth || !region.imageHeight ||
                        region.imageWidth > Math.min(4096, maxTextureSize, Math.ceil(frameWidth / 8) * 8) ||
                        region.imageHeight > Math.min(4096, maxTextureSize, Math.ceil(frameHeight / 8) * 8) ||
                        !mappingCount || mappingCount > 64 - sources.length || view.getUint16(offset + 6, false) !== 0 ||
                        header + 4 > length) { throw new Error('atlas-metadata'); }
                region.maps = [];
                var left = frameWidth;
                var top = frameHeight;
                var right = 0;
                var bottom = 0;
                for (var m = 0; m < mappingCount; m++) {
                    var mapOffset = offset + 8 + m * 12;
                    var map = {x: view.getUint16(mapOffset, false), y: view.getUint16(mapOffset + 2, false),
                        width: view.getUint16(mapOffset + 4, false), height: view.getUint16(mapOffset + 6, false),
                        atlasX: view.getUint16(mapOffset + 8, false), atlasY: view.getUint16(mapOffset + 10, false)};
                    if (map.atlasX + map.width > region.imageWidth || map.atlasY + map.height > region.imageHeight) { throw new Error('atlas-bounds'); }
                    for (var j = 0; j < region.maps.length; j++) {
                        var other = region.maps[j];
                        if (map.atlasX < other.atlasX + other.width && other.atlasX < map.atlasX + map.width &&
                                map.atlasY < other.atlasY + other.height && other.atlasY < map.atlasY + map.height) { throw new Error('atlas-overlap'); }
                    }
                    area += addSourceBox(sources, map);
                    region.maps.push(map);
                    left = Math.min(left, map.x); top = Math.min(top, map.y);
                    right = Math.max(right, map.x + map.width); bottom = Math.max(bottom, map.y + map.height);
                }
                if (region.x !== left || region.y !== top || region.width !== right - left || region.height !== bottom - top) { throw new Error('atlas-bbox'); }
                region.jpeg = new Uint8Array(data, offset + header, length - header);
            } else {
                area += addSourceBox(sources, region);
                if (region.kind === 1) {
                    if (payload[3] !== 255) { throw new Error('solid-alpha'); }
                    region.colour = payload;
                } else {
                    region.imageWidth = region.width;
                    region.imageHeight = region.height;
                    region.jpeg = payload;
                }
            }
            if (region.kind !== 1) {
                decodeArea += region.imageWidth * region.imageHeight;
                if (decodeArea > Math.floor(frameWidth * frameHeight * 5 / 4) ||
                        !jpegDimensions(region.jpeg, region.imageWidth, region.imageHeight)) { throw new Error('jpeg-budget'); }
            }
            offset += length;
            result.regions.push(region);
        }
        if (offset !== data.byteLength || (result.full && area !== frameWidth * frameHeight)) { throw new Error('coverage'); }
        return result;
    }
    function samplePixels(sequence, capturePtsUs) {
        if (!proofRequest) { return; }
        var request = proofRequest;
        proofRequest = null;
        var pixels = [];
        var rgba = new Uint8Array(4);
        for (var i = 0; i < request.points.length; i++) {
            var point = request.points[i];
            // Public coordinates are source top-left; readPixels is framebuffer bottom-left.
            gl.readPixels(point.x, frameHeight - 1 - point.y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
            pixels.push({x: point.x, y: point.y, rgba: [rgba[0], rgba[1], rgba[2], rgba[3]]});
        }
        if (!gpuOK()) { throw new Error('readback'); }
        pixelProof = {id: request.id, sequence: sequence, capturePtsUs: capturePtsUs, frameWidth: frameWidth, frameHeight: frameHeight,
            sampledAtWallMs: Date.now(), pixels: pixels, source: 'WebGL default framebuffer immediately after draw',
            panelResolutionInferred: false};
    }
    function presentReady(current, parsed, url, pairing) {
        // Host pacing and the completion ACK already bound work. Drawing here
        // avoids throttled animation-frame callbacks for GPU-only fill batches.
        if (socket !== current || !gl) { return; }
        var ready = batch && batch.completed === batch.regions.length ? batch : null;
        if (!ready && (!hasBaseline || !proofRequest)) { return; }
        try {
            gl.activeTexture(gl.TEXTURE0);
            gl.bindTexture(gl.TEXTURE_2D, texture);
            var fills = 0, filling = false;
            if (ready) {
                for (var i = 0; i < ready.regions.length; i++) {
                    var region = ready.regions[i];
                    // Texture y=0 stores source top-first rows. The final
                    // presentation quad applies the sole y inversion.
                    if (region.kind !== 1) { filling = false; }
                    if (region.kind === 1) {
                        if (!filling) {
                            openFillGPU();
                            gl.bindTexture(gl.TEXTURE_2D, null);
                            gl.bindFramebuffer(gl.FRAMEBUFFER, fillTarget);
                            gl.disable(gl.SCISSOR_TEST);
                            useQuad(fillProgram, fillPosition);
                            filling = true;
                        }
                        // Viewport quads avoid this TV's slow scissored-clear
                        // completion path without removing GPU error checks.
                        gl.viewport(region.x, region.y, region.width, region.height);
                        gl.uniform4f(fillColour, region.colour[0] / 255, region.colour[1] / 255, region.colour[2] / 255, 1);
                        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
                        fills++;
                    } else if (region.kind === 2) {
                        copyAtlas(region);
                    } else {
                        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
                        gl.bindTexture(gl.TEXTURE_2D, texture);
                        gl.texSubImage2D(gl.TEXTURE_2D, 0, region.x, region.y, gl.RGBA, gl.UNSIGNED_BYTE, region.image);
                    }
                }
            }
            gl.disable(gl.SCISSOR_TEST);
            gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            gl.viewport(0, 0, frameWidth, frameHeight);
            gl.bindTexture(gl.TEXTURE_2D, texture);
            useQuad(program, position);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
            // This WebGL 1 device has no asynchronous fence extension.
            // Bound GPU work before ACK, not just JavaScript submission.
            gl.finish();
            if (!gpuOK()) { throw new Error('draw'); }
            samplePixels(ready ? ready.sequence : lastSequence, ready ? ready.capturePtsUs : lastCapturePtsUs);
            if (!ready) { return; }
            // ACK means the complete GPU draw finished, not physical panel scanout.
            current.send(JSON.stringify({type: 'ack', sequence: ready.sequence}));
            hasBaseline = true;
            presentedFrames++;
            filledRegions += fills;
            lastSequence = ready.sequence;
            lastCapturePtsUs = ready.capturePtsUs;
            lastEncodedWallMs = ready.encodedWallMs;
            lastPresentedAt = performance.now();
            releaseBatch();
            window.clearTimeout(timeout);
            desktop.className = '';
            if (phase !== 'connected') {
                phase = 'connected';
                connectedAt = Date.now();
                setup.className = 'hidden';
                remember(url, pairing);
                reportDimensions(parsed, pairing);
            }
        } catch (error) { fail('WEBGL_PRESENT', 'The TV could not present or acknowledge the complete desktop image.'); }
    }
    function decodeNext(current, job, parsed, url, pairing) {
        if (socket !== current || batch !== job) { return; }
        // The whole batch has passed the decoded-area budget before any Image
        // exists. Each direct JPEG or atlas gets exactly one native Image.
        while (job.next < job.regions.length) {
            var region = job.regions[job.next++];
            if (region.kind === 1) { job.completed++; }
            else { decodeRegion(current, job, region, parsed, url, pairing); }
            if (socket !== current || batch !== job) { return; }
        }
        if (job.completed === job.regions.length) { presentReady(current, parsed, url, pairing); }
    }
    function decodeRegion(current, job, region, parsed, url, pairing) {
        job.active++;
        try {
            region.image = new Image();
            region.image.onload = function () {
                if (socket !== current || batch !== job) { releaseRegion(region); return; }
                if (region.image.naturalWidth !== region.imageWidth || region.image.naturalHeight !== region.imageHeight) {
                    fail('JPEG_DIMENSIONS', 'A desktop region decoded at unexpected dimensions.'); return;
                }
                region.image.onload = region.image.onerror = null;
                blobURLs.revokeObjectURL(region.url);
                region.url = null;
                region.jpeg = null;
                decodedRegions++;
                if (region.kind === 2) { decodedAtlasImages++; }
                job.active--;
                job.completed++;
                if (job.completed === job.regions.length) { presentReady(current, parsed, url, pairing); }
                else { decodeNext(current, job, parsed, url, pairing); }
            };
            region.image.onerror = function () {
                if (socket !== current || batch !== job) { releaseRegion(region); return; }
                fail('JPEG_DECODE', 'The TV could not decode a desktop JPEG region.');
            };
            region.url = blobURLs.createObjectURL(new Blob([region.jpeg], {type: 'image/jpeg'}));
            region.image.src = region.url;
        } catch (error) { fail('JPEG_DECODE', 'The TV could not allocate a desktop JPEG region.'); }
    }
    function connect(event) {
        if (event) { event.preventDefault(); }
        if (socket || document.hidden) { return; }
        if (!window.WebSocket || !blobURLs || !blobURLs.createObjectURL) {
            fail('RECEIVER_UNSUPPORTED', 'This TV does not provide the required WebGL and native JPEG receiver APIs.'); return;
        }
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
        try { current = new WebSocket(url, ['regions', 'pair.' + pairing]); current.binaryType = 'arraybuffer'; }
        catch (error) { message('The desktop WebSocket could not be opened.'); return; }
        socket = current;
        phase = 'connecting';
        frameWidth = frameHeight = sourceFPS = connectedAt = presentedFrames = decodedRegions = decodedAtlasImages = filledRegions = lastPresentedAt = 0;
        lastSequence = lastCapturePtsUs = lastEncodedWallMs = rendererError = pixelProof = null;
        hasBaseline = false;
        connectButton.disabled = true;
        connectButton.textContent = 'Connecting…';
        message('Waiting for the first full desktop image…');
        var hello = false;
        current.onopen = function () {
            if (socket === current && current.protocol !== 'regions') { fail('PROTOCOL', 'The host did not negotiate the GPU regions protocol.'); }
        };
        current.onmessage = function (event) {
            if (socket !== current) { return; }
            if (!hello) {
                var info;
                try {
                    if (typeof event.data !== 'string' || event.data.length > 4096) { throw new Error('hello'); }
                    info = JSON.parse(event.data);
                    if (!info || info.type !== 'hello' || info.codec !== 'gpu-regions' ||
                            !integer(info.width, 1, 65535) || !integer(info.height, 1, 65535) ||
                            [15, 30, 60].indexOf(info.fps) < 0 || info.viewOnly !== true || info.audio !== false) { throw new Error('hello'); }
                } catch (error) { fail('HELLO', 'The host returned an incompatible desktop regions stream.'); return; }
                frameWidth = info.width;
                frameHeight = info.height;
                sourceFPS = info.fps;
                hello = true;
                if (openGPU()) { phase = 'waiting-full'; }
                return;
            }
            if (batch) { fail('BATCH_OVERLAP', 'The host sent another image before the previous batch was acknowledged.'); return; }
            try { batch = parseBatch(event.data); }
            catch (error) { fail('BATCH_PROTOCOL', 'The host returned an invalid or incomplete desktop image batch.'); return; }
            armTimeout(current, 5000, 'The TV could not decode and present the outstanding image within five seconds.');
            decodeNext(current, batch, parsed, url, pairing);
        };
        current.onerror = function () { if (socket === current) { disconnect('Connection failed. Check the host, endpoint, pairing and TV-only firewall rule.'); } };
        current.onclose = function () { if (socket === current) { disconnect('Desktop connection closed. Start sharing locally, then Connect to reuse saved pairing.'); } };
        // Only the initial connection and an outstanding batch have timeouts. Static desktops do not.
        armTimeout(current, 30000, 'The first desktop image did not arrive. Check host GPU capture and the local network.');
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
    desktop.addEventListener('webglcontextlost', function (event) {
        event.preventDefault();
        fail('WEBGL_CONTEXT_LOST', 'The TV graphics context was lost. Reconnect when graphics are available again.');
    });
    desktop.addEventListener('webglcontextrestored', function () { message('TV graphics are available again. Connect to rebuild the full desktop image.'); });
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
        if (document.hidden) { disconnect('Disconnected while the app is in the background.'); }
        else if (savedPair && !socket) { connect(); }
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
    window.addEventListener('beforeunload', function () { disconnect('Disconnected.'); });
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
    window.OmarchyDisplayQA = {
        status: function () {
            return {phase: phase, clientReady: true, connected: phase === 'connected', viewOnly: true, audio: false,
                pairingRemembered: pairingRemembered, frameWidth: frameWidth, frameHeight: frameHeight,
                configuredFPS: sourceFPS, codec: 'gpu-regions', transport: 'regions',
                presentedFrames: presentedFrames, decodedRegions: decodedRegions, filledRegions: filledRegions, lastSequence: lastSequence,
                decodedAtlasImages: decodedAtlasImages,
                lastCapturePtsUs: lastCapturePtsUs, lastEncodedWallMs: lastEncodedWallMs,
                lastProgressAgeSeconds: lastPresentedAt ? (performance.now() - lastPresentedAt) / 1000 : null,
                sessionSeconds: connectedAt ? (Date.now() - connectedAt) / 1000 : 0,
                batchInFlight: !!batch, imageLoadsInFlight: batch ? batch.active : 0,
                loadedImagesInBatch: batch ? batch.completed : 0, maxConcurrentImageLoads: 64,
                canvasWidth: desktop.width, canvasHeight: desktop.height,
                drawingBufferWidth: gl ? gl.drawingBufferWidth : 0, drawingBufferHeight: gl ? gl.drawingBufferHeight : 0,
                maxTextureSize: maxTextureSize, maxViewportWidth: maxViewport ? maxViewport[0] : null,
                maxViewportHeight: maxViewport ? maxViewport[1] : null,
                viewportWidth: window.innerWidth, viewportHeight: window.innerHeight,
                physicalPanelWidth: null, physicalPanelHeight: null, nativePanelDetailClaimed: false,
                rendererError: rendererError, fitMode: fitMode, pixelProof: pixelProof};
        },
        requestPixels: function (points) {
            if (!socket || !hasBaseline || proofRequest || !Array.isArray(points) || !points.length || points.length > 16) { return null; }
            var copy = [];
            for (var i = 0; i < points.length; i++) {
                if (!points[i] || !integer(points[i].x, 0, frameWidth - 1) || !integer(points[i].y, 0, frameHeight - 1)) { return null; }
                copy.push({x: points[i].x, y: points[i].y});
            }
            proofRequest = {id: ++proofId, points: copy};
            // Idle evidence redraws the retained texture; it does not fabricate a received frame or ACK.
            presentReady(socket, null, null, null);
            return proofId;
        }
    };
    connectButton.disabled = false;
    fit();
    if (!savedPair) { endpoint.focus(); }
    message(storageError ? 'TV storage could not be read. Pairing may work for this session only.' :
        'Pair once. Full source pixels are retained in WebGL; TV app graphics: ' + window.innerWidth + ' × ' + window.innerHeight +
        '. This does not establish native 4K panel detail.');
    if (savedPair && !document.hidden) { connect(); }
}());
