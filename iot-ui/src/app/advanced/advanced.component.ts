import { Component, Input } from '@angular/core';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';

/// Advanced operations: Reboot, Factory Reset, Transfer (placeholder). Each
/// destructive action is Admin-gated + explicitly confirmed. The server arms a
/// /run/iot trigger that a root systemd .path unit performs (iot-httpd itself
/// is unprivileged) — see modules/http-server POST /api/v1/system/*.
@Component({
  selector: 'app-advanced',
  template: `
    <div class="page">
      <!-- ── Reboot ───────────────────────────────────────────── -->
      <ng-container *ngIf="panel==='reboot'">
        <h3>Reboot</h3>
        <p class="hint">Restart the device now. Services are briefly unavailable
          while it comes back up; configuration and credentials are preserved.</p>
        <label class="ck">
          <input type="checkbox" #rc (change)="rebootOk = rc.checked">
          I understand the device will reboot immediately.
        </label>
        <div class="act">
          <button class="btn btn-warning" [disabled]="!rebootOk || busy || !isAdmin"
                  (click)="doReboot()">{{ busy ? '…' : 'Reboot Device' }}</button>
        </div>
      </ng-container>

      <!-- ── Factory Reset ────────────────────────────────────── -->
      <ng-container *ngIf="panel==='factory'">
        <h3>Factory Reset</h3>
        <p class="warn">⚠ This <b>wipes ALL persisted configuration, credentials,
          and network settings</b>, returning the device to first-boot (Lua
          schema) defaults, then reboots. The device will likely drop off the
          network and need re-setup from the console. <b>This cannot be undone.</b></p>
        <clr-input-container>
          <label>Type <code>RESET</code> to confirm:</label>
          <input clrInput type="text" #rw (input)="resetWord = rw.value"
                 placeholder="RESET" autocomplete="off" />
        </clr-input-container>
        <div class="act">
          <button class="btn btn-danger" [disabled]="resetWord!=='RESET' || busy || !isAdmin"
                  (click)="doFactoryReset()">{{ busy ? '…' : 'Factory Reset' }}</button>
        </div>
      </ng-container>

      <!-- ── Transfer (placeholder) ───────────────────────────── -->
      <ng-container *ngIf="panel==='transfer'">
        <h3>Transfer</h3>
        <p class="hint">Coming soon — device configuration / credential transfer.</p>
      </ng-container>

      <p *ngIf="!isAdmin" class="warn">Admin access is required for these actions.</p>
      <p *ngIf="msg" class="msg">{{ msg }}</p>
    </div>
  `,
  styles: [`
    .page { padding: 24px; max-width: 640px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 12px 0; }
    .hint { color: #666; font-size: 13px; }
    .warn { color: #b71c1c; font-size: 13px; background: #fdecea; border: 1px solid #f5c6cb;
            border-radius: 4px; padding: 10px 12px; }
    .ck { display: block; margin: 14px 0; font-size: 13px; color: #444; }
    .act { margin-top: 8px; }
    .btn-warning { background: #e08e0b; color: #fff; }
    .btn-danger  { background: #c62828; color: #fff; }
    .btn:disabled { opacity: 0.5; cursor: not-allowed; }
    .msg { margin-top: 14px; font-size: 13px; color: #0072a3; }
  `]
})
export class AdvancedComponent {
  @Input() panel: 'reboot' | 'factory' | 'transfer' = 'reboot';
  rebootOk = false;
  resetWord = '';
  busy = false;
  msg = '';

  constructor(private http: HttpsvcService, private session: SessionService) {}

  get isAdmin(): boolean { return this.session.isAdmin; }

  doReboot(): void {
    this.busy = true; this.msg = '';
    this.http.systemReboot().subscribe({
      next: (r) => {
        this.busy = false;
        this.msg = r.ok ? 'Reboot requested — the device is restarting…'
                        : ('Error: ' + (r.err || 'failed'));
      },
      error: () => { this.busy = false; this.msg = 'Request sent; the device may already be rebooting.'; }
    });
  }

  doFactoryReset(): void {
    this.busy = true; this.msg = '';
    this.http.systemFactoryReset().subscribe({
      next: (r) => {
        this.busy = false;
        this.msg = r.ok ? 'Factory reset requested — wiping to defaults and rebooting…'
                        : ('Error: ' + (r.err || 'failed'));
      },
      error: () => { this.busy = false; this.msg = 'Request sent; the device may already be resetting.'; }
    });
  }
}
