import { Component, OnInit, OnDestroy, ViewChild, ElementRef } from '@angular/core';
import { Subscription, interval } from 'rxjs';
import { switchMap } from 'rxjs/operators';
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

      <!-- Log Level -->
      <h4>Log Level</h4>
      <div class="form-grid">
        <clr-select-container>
          <label>Level</label>
          <select clrSelect [disabled]="!isAdmin"
                  [ngModel]="logLevel" (ngModelChange)="setLevel($event)">
            <option *ngFor="let l of levels" [value]="l">{{ l }}</option>
          </select>
          <clr-control-helper>Affects all daemons — hot-reloaded within ~60s</clr-control-helper>
        </clr-select-container>
      </div>

      <!-- Log Output -->
      <h4 style="margin-top:32px;">Output</h4>
      <div class="log-toolbar">
        <button class="btn btn-sm" (click)="refresh()">Refresh</button>
        <label class="auto-refresh">
          <input type="checkbox" [checked]="autoRefresh" (change)="toggleAuto()" />
          auto (3s)
        </label>
      </div>
      <textarea #ta class="log-textarea" readonly
                [value]="logText"
                placeholder="No log output yet..."></textarea>
    </div>
  `,
  styles: [`
    .page { padding: 24px; display: flex; flex-direction: column; height: calc(100vh - 120px); }
    h3 { color: #333; margin: 0 0 8px 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 0 0 12px 0; font-size: 14px; font-weight: 600;
         border-bottom: 1px solid #e0e0e0; padding-bottom: 8px; }
    .log-toolbar { display: flex; align-items: center; gap: 12px; margin-bottom: 8px; }
    .btn-sm { background: #e8e8e8; border: 1px solid #d0d0d0; color: #333;
              padding: 4px 12px; border-radius: 3px; font-size: 12px; cursor: pointer; }
    .btn-sm:hover { background: #ddd; }
    .auto-refresh { font-size: 12px; color: #9e9e9e; display: flex; align-items: center; gap: 4px; cursor: pointer; margin-left: auto; }
    .log-textarea {
      flex: 1; width: 100%; background: #0d1117; color: #c9d1d9;
      border: 1px solid #30363d; border-radius: 6px; padding: 12px;
      font-family: 'SF Mono', 'Monaco', 'Menlo', monospace;
      font-size: 12px; line-height: 1.5; resize: none;
      white-space: pre; tab-size: 2; min-height: 300px;
    }
    .log-textarea:focus { outline: none; border-color: #2e7d32; }
  `]
})
export class LogViewerComponent implements OnInit, OnDestroy {
  @ViewChild('ta') ta!: ElementRef<HTMLTextAreaElement>;
  logText = '';
  autoRefresh = true;
  logLevel = 'INFO';
  levels = ['DEBUG', 'INFO', 'WARNING', 'ERROR'];
  private sub = new Subscription();
  private api = environment.apiUrl;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(
    private http: HttpClient,
    private httpSvc: HttpsvcService,
    private session: SessionService,
    private toast: ToastService
  ) {}

  ngOnInit(): void {
    // Load current log level
    this.httpSvc.dbGet(['log.level']).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const v = ((r.data as Record<string, unknown>)['log.level'] as string || 'INFO').toUpperCase();
          if (this.levels.includes(v)) this.logLevel = v;
        }
      }
    });
    // Load log text
    this.refresh();
    this.sub.add(
      interval(3000).pipe(switchMap(() => this.fetchLog())).subscribe({
        next: (text) => { if (this.autoRefresh) { this.logText = text; this.scrollBottom(); } }
      })
    );
  }

  setLevel(level: string): void {
    this.httpSvc.dbSet([{ key: 'log.level', value: level }]).subscribe({
      next: (r) => {
        if (r.ok) { this.logLevel = level; this.toast.success('Log level set to ' + level); }
        else this.toast.error(r.err || 'Failed to set log level');
      },
      error: () => this.toast.error('Failed to set log level')
    });
  }

  refresh(): void {
    this.fetchLog().subscribe({
      next: (text) => { this.logText = text; this.scrollBottom(); }
    });
  }

  toggleAuto(): void { this.autoRefresh = !this.autoRefresh; }

  private fetchLog() {
    return this.http.get(`${this.api}/api/v1/log`, {
      responseType: 'text', withCredentials: true
    });
  }

  private scrollBottom(): void {
    setTimeout(() => {
      const el = this.ta?.nativeElement;
      if (el) el.scrollTop = el.scrollHeight;
    }, 50);
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
