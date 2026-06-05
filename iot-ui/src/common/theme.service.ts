import { Injectable } from '@angular/core';

@Injectable({ providedIn: 'root' })
export class ThemeService {
  private _dark = false;

  get dark(): boolean { return this._dark; }

  init(): void {
    this._dark = localStorage.getItem('iot-theme') === 'dark';
    this.apply();
  }

  toggle(): void {
    this._dark = !this._dark;
    localStorage.setItem('iot-theme', this._dark ? 'dark' : 'light');
    this.apply();
  }

  private apply(): void {
    document.body.classList.toggle('dark-theme', this._dark);
  }
}
