import { Component, OnInit, OnDestroy, ViewChild, ElementRef } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpClient } from '@angular/common/http';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';
import { environment } from '../../environments/environment';

@Component({
  selector: 'app-log-viewer',
  template: `
    <div class="page">
      <h3>Logs</h3>

      <!-- Per-service log level (mirrors cloud-ui). "lwm2m" is implicit —
           client on the device. -->
      <h4>Log Level</h4>
      <div class="form-grid">
        <clr-select-container>
          <label>All Daemons</label>
          <select clrSelect [disabled]="!isAdmin"
                  [ngModel]="logLevel" (ngModelChange)="setLevel('log.level', $event)">
            <option *ngFor="let l of levels" [value]="l">{{ l }}</option>
          </select>
          <clr-control-helper>Global default — used when per-daemon is unset</clr-control-helper>
        </clr-select-container>
        <clr-select-container>
          <label>httpd</label>
          <select clrSelect [disabled]="!isAdmin"
                  [ngModel]="logLevelHttpd" (ngModelChange)="setLevel('log.level.httpd', $event)">
            <option *ngFor="let l of daemonLevels" [value]="l">{{ l }}</option>
          </select>
        </clr-select-container>
        <clr-select-container>
          <label>lwm2m</label>
          <select clrSelect [disabled]="!isAdmin"
                  [ngModel]="logLevelLwm2m" (ngModelChange)="setLevel('log.level.lwm2m', $event)">
            <option *ngFor="let l of daemonLevels" [value]="l">{{ l }}</option>
          </select>
        </clr-select-container>
        <clr-select-container>
          <label>vpn</label>
          <select clrSelect [disabled]="!isAdmin"
                  [ngModel]="logLevelVpn" (ngModelChange)="setLevel('log.level.vpn', $event)">
            <option *ngFor="let l of daemonLevels" [value]="l">{{ l }}</option>
          </select>
        </clr-select-container>
        <clr-select-container>
          <label>dtls</label>
          <select clrSelect [disabled]="!isAdmin"
                  [ngModel]="logLevelDtls" (ngModelChange)="setLevel('log.level.dtls', $event)">
            <option *ngFor="let l of daemonLevels" [value]="l">{{ l }}</option>
          </select>
        </clr-select-container>
      </div>

      <!-- Log output -->
      <h4 style="margin-top:28px;">Output</h4>
      <div class="log-toolbar">
        <button class="btn btn-sm" (click)="refresh()">Refresh</button>
        <button class="btn btn-sm" (click)="exportLog()" [disabled]="!logText">Export</button>
        <button class="btn btn-sm" (click)="clearLog()" [disabled]="!logText">Clear</button>
        <label class="auto-refresh">
          <input type="checkbox" [checked]="autoRefresh" (change)="toggleAuto()" />
          auto
        </label>
      </div>
      <textarea #ta class="log-textarea" readonly
                [value]="logText"
                placeholder="No log output yet..."></textarea>
    </div>
  `,
  styles: [`
    .page { padding: 16px 24px; display: flex; flex-direction: column; height: calc(100vh - 150px); }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 12px 0; }
    h4 { font-size: 13px; font-weight: 600; color: #555; margin: 0 0 10px 0; }
    .log-toolbar { display: flex; align-items: center; gap: 12px; margin-bottom: 8px; }
    .btn-sm { background: #1a5276; border: none; color: #fff; padding: 3px 10px; border-radius: 3px; cursor: pointer; font-size: 11px; }
    .btn-sm:hover { opacity: 0.8; }
    .auto-refresh { font-size: 12px; color: #9e9e9e; display: flex; align-items: center; gap: 4px; cursor: pointer; margin-left: auto; }
    .log-textarea {
      flex: 1; width: 100%; background: #0d1117; color: #c9d1d9; border: 1px solid #1a5276;
      border-radius: 4px; padding: 12px; font-family: 'SF Mono', 'Monaco', 'Menlo', monospace;
      font-size: 12px; line-height: 1.5; resize: none; overflow-y: scroll;
      white-space: pre; tab-size: 2; min-height: 240px;
    }
    .log-textarea:focus { outline: none; border-color: #2e7d32; }
  `]
})
export class LogViewerComponent implements OnInit, OnDestroy {
  @ViewChild('ta') ta!: ElementRef<HTMLTextAreaElement>;
  logText = '';
  autoRefresh = true;

  // Per-service levels (device daemons). "" = inherit global.
  logLevel = 'INFO';
  logLevelHttpd = '';
  logLevelLwm2m = '';
  logLevelVpn = '';
  logLevelDtls = '';
  levels = ['DEBUG', 'INFO', 'WARNING', 'ERROR'];
  daemonLevels: string[] = ['', 'DEBUG', 'INFO', 'WARNING', 'ERROR'];
  private readonly lvlKeys = ['log.level', 'log.level.httpd', 'log.level.lwm2m',
                              'log.level.vpn', 'log.level.dtls'];

  private active = true;
  private sub = new Subscription();
  private api = environment.apiUrl;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpClient, private httpSvc: HttpsvcService,
              private session: SessionService, private toast: ToastService) {}

  ngOnInit(): void {
    this.reloadLevels();
    this.refresh();
    this.pollLog();
  }

  // Long-poll log.version (every daemon bumps it on flush); on each tick
  // refresh the output and the levels. Re-subscribes on error so a transient
  // 401 doesn't tear the stream down.
  private pollLog(): void {
    if (!this.active) return;
    this.sub.add(
      this.httpSvc.dbGetLongPoll('log.version', 30).subscribe({
        next: () => {
          if (this.autoRefresh) { this.fetchAndShow(); this.reloadLevels(); }
          this.pollLog();
        },
        error: () => { if (this.active) setTimeout(() => this.pollLog(), 2000); }
      })
    );
  }

  setLevel(key: string, level: string): void {
    this.httpSvc.dbSet([{ key, value: level }]).subscribe({
      next: (r) => {
        if (r.ok) {
          if (key === 'log.level') this.logLevel = level;
          else if (key === 'log.level.httpd') this.logLevelHttpd = level;
          else if (key === 'log.level.lwm2m') this.logLevelLwm2m = level;
          else if (key === 'log.level.vpn') this.logLevelVpn = level;
          else if (key === 'log.level.dtls') this.logLevelDtls = level;
          this.toast.success(key + ' set to ' + (level || 'inherited'));
        } else { this.toast.error(r.err || 'Failed to set log level'); }
      },
      error: () => this.toast.error('Failed to set log level')
    });
  }

  private pickLevel(v: unknown): string {
    const s = (v as string || '').toUpperCase();
    return this.daemonLevels.includes(s) ? s : '';
  }

  private reloadLevels(): void {
    this.httpSvc.dbGet(this.lvlKeys).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          const g = (d['log.level'] as string || 'INFO').toUpperCase();
          if (this.levels.includes(g)) this.logLevel = g;
          this.logLevelHttpd = this.pickLevel(d['log.level.httpd']);
          this.logLevelLwm2m = this.pickLevel(d['log.level.lwm2m']);
          this.logLevelVpn = this.pickLevel(d['log.level.vpn']);
          this.logLevelDtls = this.pickLevel(d['log.level.dtls']);
        }
      }
    });
  }

  refresh(): void { this.fetchAndShow(); }
  toggleAuto(): void { this.autoRefresh = !this.autoRefresh; }

  /// Download the current log text as a timestamped .txt file (client-side).
  exportLog(): void {
    const blob = new Blob([this.logText], { type: 'text/plain;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `iot-log-${new Date().toISOString().replace(/[:.]/g, '-')}.txt`;
    a.click();
    URL.revokeObjectURL(url);
  }

  /// Clear the textarea view. The next refresh / auto-poll repopulates it.
  clearLog(): void { this.logText = ''; }

  private fetchAndShow(): void {
    this.http.get(`${this.api}/api/v1/log`, { responseType: 'text', withCredentials: true })
      .subscribe({ next: (text) => { this.logText = text; this.scrollBottom(); } });
  }

  private scrollBottom(): void {
    setTimeout(() => {
      const el = this.ta?.nativeElement;
      if (el) el.scrollTop = el.scrollHeight;
    }, 50);
  }

  ngOnDestroy(): void { this.active = false; this.sub.unsubscribe(); }
}
