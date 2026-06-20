import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { DataStoreService } from '../../../common/datastore.service';
import { CellStatus } from '../../../common/app-globals';

/// Cellular modem (mangOH Yellow WP) status. Reads cell.* off the shared
/// /status stream — published by the cellular-client daemon. Property/Value
/// datagrid (Project Rule 4), with a status badge on the connection state.
@Component({
  selector: 'app-cellular-status',
  template: `
    <div class="page">
      <h3>Cellular Modem</h3>
      <p class="hint" *ngIf="!hasData">
        No cellular telemetry yet — the cellular-client service publishes this
        once a WP modem is attached and enabled.
      </p>
      <clr-datagrid>
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
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    .hint { color: #888; font-size: 13px; margin: 0 0 16px 0; }
    .bars { display: inline-flex; align-items: flex-end; gap: 2px; margin-right: 8px; height: 14px; }
    .bar { width: 4px; background: #d0d5dd; border-radius: 1px; }
    .bar:nth-child(1) { height: 4px; }
    .bar:nth-child(2) { height: 6px; }
    .bar:nth-child(3) { height: 9px; }
    .bar:nth-child(4) { height: 12px; }
    .bar:nth-child(5) { height: 14px; }
    .bar.on { background: #2e7d32; }
    .sig-text { vertical-align: middle; }
  `]
})
export class CellularStatusComponent implements OnInit, OnDestroy {
  c: CellStatus = {};
  private sub = new Subscription();

  get hasData(): boolean { return !!(this.c.state || this.c.operator || this.c.signal_dbm); }
  get barsNum(): number { const n = parseInt(this.c.signal_bars || '', 10); return isNaN(n) ? 0 : n; }

  get signalText(): string {
    if (!this.c.signal_dbm) return '—';
    const bars = this.c.signal_bars ? ` (${this.c.signal_bars}/5)` : '';
    return `${this.c.signal_dbm} dBm${bars}`;
  }

  get rows(): { key: string; value: string; isBadge?: boolean; isSignal?: boolean; dsKey: string }[] {
    return [
      { key: 'State',        value: this.c.state || 'unknown', isBadge: true, dsKey: 'cell.state' },
      { key: 'Operator',     value: this.c.operator || '—',    dsKey: 'cell.operator' },
      { key: 'Technology',   value: this.c.tech || '—',        dsKey: 'cell.tech' },
      { key: 'Registration', value: this.c.reg || '—',         dsKey: 'cell.reg' },
      { key: 'Signal',       value: this.signalText, isSignal: true, dsKey: 'cell.signal.dbm' },
      { key: 'IP Address',   value: this.c.ip || '—',          dsKey: 'cell.ip' },
      { key: 'SIM ICCID',    value: this.c.iccid || '—',       dsKey: 'cell.iccid' },
    ];
  }

  constructor(private ds: DataStoreService) {}

  ngOnInit(): void {
    this.sub.add(this.ds.observeStatus().subscribe((s) => { this.c = s.cell || {}; }));
  }
  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
