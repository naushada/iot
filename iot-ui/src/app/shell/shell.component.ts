import {
  Component, AfterViewInit, OnDestroy, ElementRef, ViewChild, HostListener
} from '@angular/core';
import { Terminal } from 'xterm';
import { FitAddon } from 'xterm-addon-fit';
import { HttpsvcService } from '../../common/httpsvc.service';

// ── byte ⇄ base64 helpers ──────────────────────────────────────────────
// PTY traffic is raw bytes (control codes, possibly multibyte UTF-8), so it
// rides the JSON API base64-encoded. We deal in Uint8Array on both ends and
// let xterm do its own UTF-8 decoding on write().
function bytesToB64(u8: Uint8Array): string {
  let s = '';
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return btoa(s);
}
function b64ToBytes(b64: string): Uint8Array {
  const bin = atob(b64);
  const u8 = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
  return u8;
}

@Component({
  selector: 'app-shell',
  template: `
    <div class="page">
      <div class="head">
        <h3>Terminal</h3>
        <span class="state" [class.up]="connected" [class.down]="!connected">
          {{ connected ? 'connected' : (closed ? 'closed' : 'connecting…') }}
        </span>
        <span class="spacer"></span>
        <button class="btn" (click)="restart()" [disabled]="!closed && connected===false">
          <clr-icon shape="refresh"></clr-icon> New session
        </button>
      </div>
      <div #term class="term"></div>
      <p class="hint">
        Shell on this device — runs as the <code>iot-httpd</code> service user,
        <strong>not root</strong> (so <code>ping</code>, package installs, and
        writes under <code>/etc</code> lack privilege). Admin-only; session ends
        on idle or when you leave the page.
      </p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; display: flex; flex-direction: column; height: 100%;
            box-sizing: border-box; }
    .head { display: flex; align-items: center; gap: 12px; margin: 0 0 12px 0; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0; }
    .state { font-size: 12px; padding: 1px 10px; border-radius: 10px;
             background: #eef2f7; color: #555; }
    .state.up { background: #dff5e3; color: #1b7a37; }
    .state.down { background: #fdecea; color: #b3261e; }
    .spacer { flex: 1; }
    .btn { display: inline-flex; align-items: center; gap: 6px; border: 1px solid #ccc;
           background: #fff; border-radius: 4px; padding: 4px 10px; cursor: pointer;
           font-size: 13px; }
    .btn:disabled { opacity: .5; cursor: default; }
    .term { flex: 1; min-height: 360px; background: #1e1e1e; border-radius: 6px;
            padding: 8px; overflow: hidden; }
    .hint { font-size: 12px; color: #888; margin: 10px 0 0 0; }
  `]
})
export class ShellComponent implements AfterViewInit, OnDestroy {
  @ViewChild('term', { static: true }) termRef!: ElementRef<HTMLDivElement>;

  private term?: Terminal;
  private fit = new FitAddon();
  private sid = '';
  private alive = true;            // component still mounted
  connected = false;
  closed = false;

  constructor(private http: HttpsvcService) {}

  ngAfterViewInit(): void {
    this.term = new Terminal({
      fontFamily: 'ui-monospace, SFMono-Regular, Menlo, monospace',
      fontSize: 13,
      cursorBlink: true,
      theme: { background: '#1e1e1e' }
    });
    this.term.loadAddon(this.fit);
    this.term.open(this.termRef.nativeElement);
    this.safeFit();

    // Keystrokes → PTY (UTF-8 → base64). Fire-and-forget; the echoed
    // output comes back through the long-poll.
    this.term.onData((data) => {
      if (!this.sid) return;
      const b64 = bytesToB64(new TextEncoder().encode(data));
      this.http.shellInput(this.sid, b64).subscribe({ error: () => {} });
    });

    this.open();
  }

  private open(): void {
    this.closed = false;
    this.connected = false;
    const { cols, rows } = this.dims();
    this.http.shellOpen(cols, rows).subscribe({
      next: (r) => {
        if (!this.alive) return;
        if (r.ok && r.sid) {
          this.sid = r.sid;
          this.connected = true;
          this.poll(r.sid);
        } else {
          this.term?.writeln(`\r\n\x1b[31m[shell unavailable: ${r.err || 'error'}]\x1b[0m`);
        }
      },
      error: () => this.term?.writeln('\r\n\x1b[31m[failed to open shell]\x1b[0m')
    });
  }

  // Single outstanding output long-poll; re-subscribe on each response.
  // `sid` is captured per call so a late response from a previous session
  // (e.g. after New session) can't clobber the current one.
  private poll(sid: string): void {
    if (!this.alive || this.sid !== sid) return;
    this.http.shellOutput(sid).subscribe({
      next: (r) => {
        if (!this.alive || this.sid !== sid) return;
        if (r.data) this.term?.write(b64ToBytes(r.data));
        if (r.closed) {
          this.connected = false;
          this.closed = true;
          this.sid = '';
          this.term?.writeln('\r\n\x1b[33m[session closed]\x1b[0m');
          return;
        }
        this.poll(sid);
      },
      error: () => {
        if (!this.alive || this.sid !== sid) return;
        // Transient network/long-poll hiccup — back off briefly and retry
        // while the session is presumed alive.
        setTimeout(() => this.poll(sid), 1500);
      }
    });
  }

  restart(): void {
    if (this.sid) { this.http.shellClose(this.sid).subscribe({ error: () => {} }); }
    this.term?.reset();
    this.sid = '';
    this.open();
  }

  @HostListener('window:resize')
  onResize(): void { this.safeFit(); }

  private safeFit(): void {
    try { this.fit.fit(); } catch { /* element not laid out yet */ }
    if (this.sid) {
      const { cols, rows } = this.dims();
      this.http.shellResize(this.sid, cols, rows).subscribe({ error: () => {} });
    }
  }

  private dims(): { cols: number; rows: number } {
    return { cols: this.term?.cols || 80, rows: this.term?.rows || 24 };
  }

  ngOnDestroy(): void {
    this.alive = false;
    if (this.sid) this.http.shellClose(this.sid).subscribe({ error: () => {} });
    this.term?.dispose();
  }
}
