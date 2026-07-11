import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { DataStoreService } from '../../../common/datastore.service';
import { RouteEntry, IfaceEntry, RoutingStatus, WanStatus } from '../../../common/app-globals';

/// Routing → Routes: the device's live routing information — kernel routing
/// table, interface list, default gateway and resolvers. Published by
/// net-router every poll tick (net.routes / net.ifaces / net.dns) and read
/// off the shared /status stream. Clarity clr-datagrid (Project Rule 4).
@Component({
  selector: 'app-route-table',
  template: `
    <div class="page">
      <h3>Routes</h3>

      <clr-datagrid class="summary">
        <clr-dg-column>Property</clr-dg-column>
        <clr-dg-column>Value</clr-dg-column>
        <clr-dg-row>
          <clr-dg-cell>Router State <app-ds-hint *dsDebug key="net.state"></app-ds-hint></clr-dg-cell>
          <clr-dg-cell><app-status-badge [label]="r.state || 'unknown'" [state]="r.state || ''"></app-status-badge></clr-dg-cell>
        </clr-dg-row>
        <clr-dg-row>
          <clr-dg-cell>Active Interface <app-ds-hint *dsDebug key="net.iface.active"></app-ds-hint></clr-dg-cell>
          <clr-dg-cell>{{ wan.active_iface || '—' }}</clr-dg-cell>
        </clr-dg-row>
        <clr-dg-row>
          <clr-dg-cell>Default Gateway</clr-dg-cell>
          <clr-dg-cell>{{ defaultGateway || '—' }}</clr-dg-cell>
        </clr-dg-row>
        <clr-dg-row>
          <clr-dg-cell>DNS <app-ds-hint *dsDebug key="net.dns"></app-ds-hint></clr-dg-cell>
          <clr-dg-cell>{{ r.dns || '—' }}</clr-dg-cell>
        </clr-dg-row>
      </clr-datagrid>

      <h4>Routing Table <app-ds-hint *dsDebug key="net.routes"></app-ds-hint></h4>
      <clr-datagrid>
        <clr-dg-column>Destination</clr-dg-column>
        <clr-dg-column>Gateway</clr-dg-column>
        <clr-dg-column>Interface</clr-dg-column>
        <clr-dg-column>Protocol</clr-dg-column>
        <clr-dg-column>Scope</clr-dg-column>
        <clr-dg-column>Source</clr-dg-column>
        <clr-dg-column>Metric</clr-dg-column>

        <clr-dg-placeholder>No routes published yet — the net-router service refreshes these every few seconds.</clr-dg-placeholder>

        <clr-dg-row *clrDgItems="let rt of routes">
          <clr-dg-cell>{{ rt.dst || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ rt.gateway || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ rt.dev || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ rt.proto || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ rt.scope || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ rt.prefsrc || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ rt.metric || '—' }}</clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ routes.length }} route{{ routes.length === 1 ? '' : 's' }}</clr-dg-footer>
      </clr-datagrid>

      <h4>Interfaces <app-ds-hint *dsDebug key="net.ifaces"></app-ds-hint></h4>
      <clr-datagrid>
        <clr-dg-column>Name</clr-dg-column>
        <clr-dg-column>State</clr-dg-column>
        <clr-dg-column>IPv4</clr-dg-column>
        <clr-dg-column>MAC</clr-dg-column>

        <clr-dg-placeholder>No interfaces published yet.</clr-dg-placeholder>

        <clr-dg-row *clrDgItems="let i of ifaces">
          <clr-dg-cell>{{ i.name || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ i.state || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ i.ip || '—' }}</clr-dg-cell>
          <clr-dg-cell>{{ i.mac || '—' }}</clr-dg-cell>
        </clr-dg-row>

        <clr-dg-footer>{{ ifaces.length }} interface{{ ifaces.length === 1 ? '' : 's' }}</clr-dg-footer>
      </clr-datagrid>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { font-size: 16px; font-weight: 600; color: #333; margin: 0 0 20px 0; }
    h4 { font-size: 14px; font-weight: 600; color: #333; margin: 24px 0 10px 0; }
    .summary { max-width: 560px; }
  `]
})
export class RouteTableComponent implements OnInit, OnDestroy {
  r: RoutingStatus = {};
  wan: WanStatus = {};
  private sub = new Subscription();

  get routes(): RouteEntry[] { return Array.isArray(this.r.routes) ? this.r.routes : []; }
  get ifaces(): IfaceEntry[] { return Array.isArray(this.r.ifaces) ? this.r.ifaces : []; }

  get defaultGateway(): string {
    const d = this.routes.find(rt => rt.dst === 'default' && rt.gateway);
    return d?.gateway || '';
  }

  constructor(private ds: DataStoreService) {}

  ngOnInit(): void {
    this.sub.add(this.ds.observeStatus().subscribe((s) => {
      this.r = s.routing || {};
      this.wan = s.wan || {};
    }));
  }
  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
