import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { DataStoreService } from '../../../common/datastore.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';
import { CellStatus, SmsInboxEntry } from '../../../common/app-globals';

/// Cellular modem (mangOH Yellow WP) status. Reads cell.* off the shared
/// /status stream — published by the cellular-client daemon. Property/Value
/// datagrid (Project Rule 4), with a status badge on the connection state.
@Component({
  selector: 'app-cellular-status',
  template: `
    <div class="page">
      <div class="head">
        <h3>Cellular Modem</h3>
        <button *ngIf="isAdmin" class="btn btn-sm btn-warning-outline"
                [disabled]="restarting || isOffline"
                (click)="restartModule()">
          {{ restarting ? 'Restarting…' : 'Restart Module' }}
        </button>
      </div>
      <p class="hint" *ngIf="!hasData">
        No cellular telemetry yet — the cellular-client service publishes this
        once a WP modem is attached and enabled.
      </p>
      <div class="offline" *ngIf="isOffline">
        <clr-icon shape="disconnect"></clr-icon>
        Modem not detected — it may be powered off or unplugged. The values below
        are the last known readings.
      </div>
      <clr-datagrid class="panel">
        <clr-dg-column>Property</clr-dg-column>
        <clr-dg-column>Value</clr-dg-column>

        <clr-dg-row *clrDgItems="let row of rows">
          <clr-dg-cell>
            {{ row.key }}
            <app-ds-hint *dsDebug [key]="row.dsKey"></app-ds-hint>
          </clr-dg-cell>
          <clr-dg-cell>
            <app-status-badge *ngIf="row.isBadge" [label]="row.value" [state]="c.state || ''"></app-status-badge>
            <ng-container *ngIf="row.isSignal">
              <span class="bars" *ngIf="barsNum > 0">
                <span class="bar" *ngFor="let b of [1,2,3,4,5]" [class.on]="b <= barsNum"></span>
              </span>
              <span class="sig-text">{{ row.value }}</span>
            </ng-container>
            <span *ngIf="!row.isBadge && !row.isSignal">{{ row.value }}</span>
          </clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ rows.length }} properties</clr-dg-footer>
      </clr-datagrid>

      <div class="inbox">
        <div class="inbox-head">
          <h4>Received SMS</h4>
          <button *ngIf="isAdmin && smsInbox.length" class="btn btn-sm btn-outline"
                  [disabled]="clearing" (click)="clearSms()">
            {{ clearing ? 'Clearing…' : 'Clear' }}
          </button>
        </div>
        <clr-datagrid>
          <clr-dg-column [style.width.px]="170">Time</clr-dg-column>
          <clr-dg-column [style.width.px]="150">Sender</clr-dg-column>
          <clr-dg-column>Message</clr-dg-column>

          <clr-dg-placeholder>No SMS received yet.</clr-dg-placeholder>

          <clr-dg-row *clrDgItems="let m of smsInbox">
            <clr-dg-cell>{{ m.ts || '—' }}</clr-dg-cell>
            <clr-dg-cell>{{ m.from || '—' }}</clr-dg-cell>
            <clr-dg-cell class="msg-cell">{{ m.text || '—' }}</clr-dg-cell>
          </clr-dg-row>

          <clr-dg-footer>{{ smsInbox.length }} message{{ smsInbox.length === 1 ? '' : 's' }}</clr-dg-footer>
        </clr-datagrid>
      </div>

      <div class="send" *ngIf="isAdmin">
        <h4>Send SMS</h4>
        <div class="row">
          <input clrInput placeholder="+CountryNumber" [(ngModel)]="smsTo" name="smsTo" />
          <input clrInput class="msg" placeholder="Message" [(ngModel)]="smsText" name="smsText" />
          <button class="btn btn-sm btn-primary" [disabled]="sending || !smsTo || !smsText"
                  (click)="sendSms()">{{ sending ? 'Sending…' : 'Send' }}</button>
        </div>
        <p class="send-status" *ngIf="c.sms_send_status">
          Status: {{ c.sms_send_status }}
        </p>
        <p class="hint">Mobile-originated via the modem (AT+CMGS). Delivery depends on the SIM's SMS plan.</p>
      </div>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    .head { display: flex; align-items: center; justify-content: space-between;
            margin: 0 0 20px 0; width: 100%; }
    .head h3 { margin: 0; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    .hint { color: #888; font-size: 13px; margin: 0 0 16px 0; }
    .offline { display: flex; align-items: center; gap: 6px; margin: 0 0 16px 0;
               padding: 8px 12px; border-radius: 4px; font-size: 13px;
               color: #944; background: #fdf3f3; border: 1px solid #f0d0d0; }
    .offline clr-icon { flex: none; }
    .bars { display: inline-flex; align-items: flex-end; gap: 2px; margin-right: 8px; height: 14px; }
    .bar { width: 4px; background: #d0d5dd; border-radius: 1px; }
    .bar:nth-child(1) { height: 4px; }
    .bar:nth-child(2) { height: 6px; }
    .bar:nth-child(3) { height: 9px; }
    .bar:nth-child(4) { height: 12px; }
    .bar:nth-child(5) { height: 14px; }
    .bar.on { background: #2e7d32; }
    .sig-text { vertical-align: middle; }
    /* Every block spans the full page width: the property table, the SMS
       history table and the Send card. They were 100% / 720px / 640px, so
       nothing lined up. */
    .panel { width: 100%; }
    .inbox { margin-top: 24px; width: 100%; }
    .inbox-head { display: flex; align-items: center; justify-content: space-between; }
    .inbox-head h4 { margin: 0 0 10px 0; }
    .inbox h4 { font-size: 14px; font-weight: 600; color: #333; margin: 0 0 10px 0; }
    .msg-cell { white-space: pre-wrap; word-break: break-word; }
    .send { margin-top: 24px; width: 100%; }
    .send h4 { font-size: 14px; font-weight: 600; color: #333; margin: 0 0 10px 0; }
    .send .row { display: flex; gap: 8px; align-items: center; }
    .send .msg { flex: 1; }
    .send-status { font-size: 13px; color: #555; margin: 8px 0 0 0; }
    .hint { color: #888; font-size: 12px; margin: 6px 0 0 0; }
  `]
})
export class CellularStatusComponent implements OnInit, OnDestroy {
  c: CellStatus = {};
  smsTo = '';
  smsText = '';
  sending = false;
  restarting = false;
  clearing = false;
  private sub = new Subscription();

  get isAdmin(): boolean { return this.session.isAdmin; }

  restartModule(): void {
    if (!this.isAdmin) return;
    this.restarting = true;
    // Same token idiom as Send SMS: the daemon watches cell.reset.request and
    // cycles the radio (AT+CFUN=0/1) + re-applies APN/SMS/RAT. Progress shows
    // in the State badge (init → searching → registered → connected).
    this.ds.write([
      { key: 'cell.reset.request', value: String(Date.now()) },
    ]).subscribe({
      next: (r) => {
        this.restarting = false;
        if (r.ok) this.toast.success('Module restart requested — watch the State badge');
        else this.toast.error('Restart request failed');
      },
      error: () => { this.restarting = false; this.toast.error('Restart request failed'); },
    });
  }

  sendSms(): void {
    if (!this.isAdmin || !this.smsTo || !this.smsText) return;
    this.sending = true;
    // Set the envelope, then bump the request token (unique) to trigger the send.
    this.ds.write([
      { key: 'sms.send.to',      value: this.smsTo },
      { key: 'sms.send.text',    value: this.smsText },
      { key: 'sms.send.request', value: String(Date.now()) },
    ]).subscribe({
      next: (r) => {
        this.sending = false;
        if (r.ok) { this.toast.success('SMS queued — see status below'); this.smsText = ''; }
        else this.toast.error('Send failed');
      },
      error: () => { this.sending = false; this.toast.error('Send failed'); },
    });
  }

  /// Clear the received-SMS history. This is a COMMAND, not a ds wipe: the
  /// daemon owns the inbox in memory and would republish it on the next message,
  /// so we bump sms.clear.request and it clears + republishes an empty inbox.
  clearSms(): void {
    if (!this.isAdmin) return;
    this.clearing = true;
    this.ds.write([{ key: 'sms.clear.request', value: String(Date.now()) }]).subscribe({
      next: (r) => {
        this.clearing = false;
        if (r.ok) this.toast.success('SMS history cleared');
        else this.toast.error('Clear failed');
      },
      error: () => { this.clearing = false; this.toast.error('Clear failed'); },
    });
  }

  get hasData(): boolean { return !!(this.c.state || this.c.operator || this.c.signal_dbm); }
  // The daemon publishes cell.state="absent" (and clears the live fields) when the
  // modem tty tears down. "" is a cold start that never saw a modem.
  get isOffline(): boolean {
    const s = (this.c.state || '').toLowerCase();
    return s === 'absent' || s === 'sim-missing';
  }
  get barsNum(): number { const n = parseInt(this.c.signal_bars || '', 10); return isNaN(n) ? 0 : n; }

  get signalText(): string {
    if (!this.c.signal_dbm) return '—';
    const bars = this.c.signal_bars ? ` (${this.c.signal_bars}/5)` : '';
    return `${this.c.signal_dbm} dBm${bars}`;
  }

  get rows(): { key: string; value: string; isBadge?: boolean; isSignal?: boolean; dsKey: string }[] {
    const rows = [
      { key: 'State',        value: this.c.state || 'unknown', isBadge: true, dsKey: 'cell.state' },
      { key: 'Operator',     value: this.c.operator || '—',    dsKey: 'cell.operator' },
      { key: 'Technology',   value: this.c.tech || '—',        dsKey: 'cell.tech' },
      { key: 'Registration', value: this.c.reg || '—',         dsKey: 'cell.reg' },
      { key: 'Reg (CS / SMS)', value: this.c.reg_cs || '—',    dsKey: 'cell.reg.cs' },
      { key: 'Reg (PS / Data)', value: this.c.reg_ps || '—',   dsKey: 'cell.reg.ps' },
      { key: 'Reg (LTE)',    value: this.c.reg_eps || '—',     dsKey: 'cell.reg.eps' },
      { key: 'Signal',       value: this.signalText, isSignal: true, dsKey: 'cell.signal.dbm' },
      { key: 'RAT',          value: this.c.rat || '—',         dsKey: 'cell.rat.current' },
      { key: 'Capability',   value: this.c.capability || '—',  dsKey: 'cell.capability' },
      { key: 'APN',          value: this.c.apn || '—',         dsKey: 'cell.apn.current' },
      { key: 'IP Address',   value: this.c.ip || '—',          dsKey: 'cell.ip' },
      { key: 'DNS',          value: this.c.dns || '—',         dsKey: 'cell.dns' },
      { key: 'SIM ICCID',    value: this.c.iccid || '—',       dsKey: 'cell.iccid' },
      { key: 'IMEI',         value: this.c.imei || '—',        dsKey: 'cell.imei' },
      { key: 'MSISDN',       value: this.c.msisdn || '—',      dsKey: 'cell.msisdn' },
      { key: 'Model',        value: this.c.model || '—',       dsKey: 'cell.model' },
      { key: 'Firmware',     value: this.c.fw || '—',          dsKey: 'cell.fw' },
    ];
    // Network reject reason — only surfaced when present and not registered.
    if (this.c.reg_reason) {
      rows.push({ key: 'Reject Reason', value: this.c.reg_reason, dsKey: 'cell.reg.reason' });
    }
    // Received-SMS total — the messages themselves render in the table below.
    if (this.c.sms_count && this.c.sms_count !== '0') {
      rows.push({ key: 'SMS Received', value: this.c.sms_count, dsKey: 'sms.count' });
    }
    // SIM message-store usage — a full store silently blocks MT-SMS delivery.
    if (this.c.sms_storage) {
      rows.push({ key: 'SIM SMS Storage', value: this.c.sms_storage, dsKey: 'sms.storage' });
    }
    return rows;
  }

  get smsInbox(): SmsInboxEntry[] {
    return Array.isArray(this.c.sms_inbox) ? this.c.sms_inbox : [];
  }

  constructor(private ds: DataStoreService,
              private session: SessionService,
              private toast: ToastService) {}

  ngOnInit(): void {
    this.sub.add(this.ds.observeStatus().subscribe((s) => { this.c = s.cell || {}; }));
  }
  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
