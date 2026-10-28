/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Omarchy Display inspector integration, 2026-10-28; added to Jellyfin Web. */
import { ServerConnections } from 'lib/jellyfin-apiclient';

// Inspector-only, pull-based status. No background requests, logging, storage
// enumeration, raw API objects, endpoint/user/device identifiers or tokens.
window.JellyfinTVStatus = {
    snapshot: function () {
        var client = ServerConnections.currentApiClient();
        var video = document.querySelector('video');
        var media = video || document.createElement('video');
        var chrome = /Chrome\/(\d+)/.exec(navigator.userAgent);
        return {
            webVersion: '12.2.0',
            compatibility: 'tizen3-static-palette',
            documentReady: document.readyState,
            chromiumMajor: chrome ? Number(chrome[1]) : null,
            nativeTizen: typeof window.tizen !== 'undefined',
            nativeShell: !!window.NativeShell,
            apiConfigured: !!client,
            authenticatedLocally: !!(client && client.isLoggedIn()),
            authenticationNote: 'Local session flag only; does not validate token against server',
            focusedElement: document.activeElement ? document.activeElement.tagName : null,
            nativeHls: !!(media.canPlayType('application/vnd.apple.mpegURL') || media.canPlayType('application/x-mpegURL')),
            media: video ? {
                sourceAssigned: !!video.currentSrc,
                paused: video.paused,
                ended: video.ended,
                readyState: video.readyState,
                networkState: video.networkState,
                currentTime: isFinite(video.currentTime) ? video.currentTime : null,
                duration: isFinite(video.duration) ? video.duration : null,
                width: video.videoWidth,
                height: video.videoHeight,
                errorCode: video.error ? video.error.code : null
            } : null
        };
    },
    probe: function () {
        var client = ServerConnections.currentApiClient();
        if (!client) {
            return Promise.resolve({ reachable: false, reason: 'no-selected-server' });
        }
        return new Promise(function (resolve) {
            var request = new XMLHttpRequest();
            // Public endpoint: no authentication header or token is required.
            request.open('GET', client.serverAddress().replace(/\/$/, '') + '/System/Info/Public', true);
            request.timeout = 8000;
            request.onload = function () {
                var version = null;
                if (request.status >= 200 && request.status < 300) {
                    try {
                        var response = JSON.parse(request.responseText);
                        if (typeof response.Version === 'string' && /^\d+(\.\d+){1,3}$/.test(response.Version)) {
                            version = response.Version;
                        }
                    } catch (error) {
                        resolve({ reachable: false, status: request.status, reason: 'invalid-public-api-response' });
                        return;
                    }
                }
                resolve({ reachable: request.status >= 200 && request.status < 300, status: request.status, serverVersion: version });
            };
            request.onerror = function () { resolve({ reachable: false, reason: 'network-or-certificate-error' }); };
            request.ontimeout = function () { resolve({ reachable: false, reason: 'timeout' }); };
            request.send();
        });
    }
};
