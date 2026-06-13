import { Component, Input, OnInit } from '@angular/core';
import { HttpsvcService } from './httpsvc.service';
import { SessionService } from './session.service';

/// Debug aid rendered just below a form input (inside a clr-control-helper):
/// shows the data-store KEY that fills the field and an editable RAW value box
/// bound straight to that key. Lets an Admin building their own app see the
/// exact ds key/value behind every field and poke it directly.
///
/// Self-contained — it fetches and writes the raw value itself via
/// /api/v1/db, so wiring a field is just:
///   <clr-control-helper *dsDebug>
///     <app-ds-hint key="cloud.vpn.subnet"></app-ds-hint>
///   </clr-control-helper>
/// Only instantiated while Debug mode is on (see DsDebugDirective), so it
/// costs nothing when off. Mirrors the services-list `.svc-key` styling.
@Component({
  selector: 'app-ds-hint',
  template: `
    <code class="ds-key" [title]="key">{{ key }}</code>
    <input class="ds-raw" [value]="raw" [disabled]="!isAdmin"
           [title]="'Raw value of ' + key"
           (change)="save($any($event.target).value)" />
    <span class="ds-msg" *ngIf="msg">{{ msg }}</span>
  `,
  styles: [`
    :host { display: block; margin-top: 2px; }
    .ds-key { display: block; font-size: 11px; color: #9e9e9e; font-family: monospace; }
    .ds-raw {
      display: block; width: 100%; box-sizing: border-box; margin-top: 2px;
      font-size: 11px; font-family: monospace; padding: 2px 6px;
      border: 1px solid #c9c9c9; border-radius: 3px; background: #fafafa; color: #333;
    }
    .ds-raw:disabled { opacity: .6; cursor: not-allowed; }
    .ds-msg { font-size: 10px; color: #66bb6a; margin-left: 2px; }
  `]
})
export class DsHintComponent implements OnInit {
  @Input() key = '';
  raw = '';
  msg = '';

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private session: SessionService) {}

  ngOnInit(): void {
    if (!this.key) return;
    this.http.dbGet([this.key]).subscribe({
      next: (r) => {
        const v = r.ok && r.data ? r.data[this.key] : undefined;
        this.raw = (v === undefined || v === null) ? '' : String(v);
      }
    });
  }

  save(value: string): void {
    if (!this.isAdmin || !this.key) return;
    this.http.dbSet([{ key: this.key, value }]).subscribe({
      next: (r) => {
        this.raw = value;
        this.msg = r.ok ? 'saved' : (r.err || 'error');
        if (r.ok) setTimeout(() => { this.msg = ''; }, 1500);
      },
      error: () => { this.msg = 'error'; }
    });
  }
}
