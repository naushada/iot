import { Component, OnInit, OnDestroy, ViewChild, ElementRef } from '@angular/core';
import { Subscription, interval } from 'rxjs';
import { switchMap } from 'rxjs/operators';
import { HttpClient } from '@angular/common/http';
import { environment } from '../../environments/environment';

@Component({
  selector: 'app-log-viewer',
  template: `
    <div class="log-viewer">
      <div class="log-header">
        <span class="title">Log Output</span>
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
    .log-viewer { padding: 16px; display: flex; flex-direction: column; height: calc(100vh - 160px); }
    .log-header { display: flex; align-items: center; gap: 12px; margin-bottom: 8px; }
    .title { font-size: 15px; font-weight: 600; color: #e0e0e0; }
    .btn-sm { background: #1a5276; border: none; color: #e0e0e0; padding: 3px 10px; border-radius: 3px; cursor: pointer; font-size: 11px; }
    .btn-sm:hover { opacity: 0.8; }
    .auto-refresh { font-size: 12px; color: #9e9e9e; display: flex; align-items: center; gap: 4px; cursor: pointer; margin-left: auto; }
    .log-textarea {
      flex: 1; width: 100%; background: #0d1117; color: #c9d1d9; border: 1px solid #1a5276;
      border-radius: 4px; padding: 12px; font-family: 'SF Mono', 'Monaco', 'Menlo', monospace;
      font-size: 12px; line-height: 1.5; resize: none; overflow-y: scroll;
      white-space: pre; tab-size: 2;
    }
    .log-textarea:focus { outline: none; border-color: #2e7d32; }
  `]
})
export class LogViewerComponent implements OnInit, OnDestroy {
  @ViewChild('ta') ta!: ElementRef<HTMLTextAreaElement>;
  logText = '';
  autoRefresh = true;
  private sub = new Subscription();
  private api = environment.apiUrl;

  constructor(private http: HttpClient) {}

  ngOnInit(): void {
    this.refresh();
    this.sub.add(
      interval(3000).pipe(switchMap(() => this.fetchLog())).subscribe({
        next: (text) => { if (this.autoRefresh) { this.logText = text; this.scrollBottom(); } }
      })
    );
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
