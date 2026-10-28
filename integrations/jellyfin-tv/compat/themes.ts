// Omarchy Display adaptation, 2026-10-28, of Jellyfin Web's src/themes/index.ts (GPL-2.0-or-later).
// Chromium 47 cannot resolve MUI's runtime CSS custom properties. Keep every
// upstream palette, but let Emotion emit literal values from ordinary themes.
import { createTheme, type Theme } from '@mui/material/styles';
import { useEffect, useState } from 'react';

import Events, { type Event } from 'utils/events';
import { EventType } from 'constants/eventType';
import { DEFAULT_THEME_OPTIONS } from './_base/theme';
import appletv from './appletv';
import blueradiance from './blueradiance';
import dark from './dark';
import light from './light';
import purplehaze from './purplehaze';
import wmc from './wmc';

const schemes = { appletv, blueradiance, dark, light, purplehaze, wmc };
const cache: Record<string, Theme> = {};

export function getLegacyTheme(id: string): Theme {
    const key = Object.prototype.hasOwnProperty.call(schemes, id) ? id : 'dark';
    if (!cache[key]) {
        cache[key] = createTheme({
            ...DEFAULT_THEME_OPTIONS,
            ...schemes[key as keyof typeof schemes],
            cssVariables: false
        });
    }
    return cache[key];
}

export function useLegacyTheme(requested?: string): Theme {
    const [current, setCurrent] = useState(() => document.documentElement.getAttribute('data-theme') || 'dark');
    useEffect(() => {
        const changed = (_event: Event, value: string) => setCurrent(value);
        Events.on(document, EventType.THEME_CHANGE, changed);
        return () => Events.off(document, EventType.THEME_CHANGE, changed);
    }, []);
    return getLegacyTheme(requested || current);
}

export default getLegacyTheme('dark');
