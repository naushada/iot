import { Injectable } from '@angular/core';

/// UI "Debug" mode — a developer aid that, when on, reveals beneath every
/// config form input the data-store key that backs it plus an editable raw
/// value box, so an Admin building their own app can see and poke the
/// underlying ds key/value. Mirrors ThemeService exactly: per-browser,
/// client-side only, persisted to localStorage under `iot-debug`.
@Injectable({ providedIn: 'root' })
export class DebugService {
  private _on = false;

  get on(): boolean { return this._on; }

  init(): void {
    this._on = localStorage.getItem('iot-debug') === 'on';
  }

  toggle(): void {
    this._on = !this._on;
    localStorage.setItem('iot-debug', this._on ? 'on' : 'off');
  }
}
