import { Component } from '@angular/core';
import { Router } from '@angular/router';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { PubSubService } from '../../common/pubsubsvc.service';
import { ThemeService } from '../../common/theme.service';
import { DebugService } from '../../common/debug.service';
import { DataStoreService } from '../../common/datastore.service';
import { StatusSnapshot, fmtDuration } from '../../common/app-globals';

@Component({
  selector: 'app-main',
  templateUrl: './main.component.html',
  styleUrls: ['./main.component.scss']
})
export class MainComponent {
  selectedMenu = 'dashboard';
  selectedSubNav = '';      // submenu item label
  selectedSubItem = '';     // event from submenu
  today = new Date().toLocaleDateString('en-US', {
    weekday: 'short', year: 'numeric', month: 'short', day: 'numeric'
  });

  status: StatusSnapshot | null = null;

  // Sidebar nav items with collapsible children
  menus = [
    { id: 'dashboard', label: 'Dashboard', svg: 'assets/icons/dashboard.svg', children: [] as {id:string,label:string}[] },
    { id: 'vpn',       label: 'VPN',       svg: 'assets/icons/vpn.svg',
      children: [{id:'config',label:'Configuration'},{id:'status',label:'Status'}] },
    { id: 'http',      label: 'HTTP',      svg: 'assets/icons/http.svg', children: [] as {id:string,label:string}[] },
    { id: 'wan',       label: 'WAN',       svg: 'assets/icons/wan.svg',
      children: [{id:'wifi',label:'WiFi Config'},{id:'scan',label:'Scan Results'},{id:'priority',label:'Priority'},{id:'cellular',label:'Cellular Config'},{id:'cellstatus',label:'Cellular Status'}] },
    { id: 'sensors',   label: 'Sensors',   svg: 'assets/icons/sensor.svg',
      children: [{id:'env',label:'Sensors'},{id:'gps',label:'Location (GPS)'}] },
    { id: 'vehicle',   label: 'Vehicle',   svg: 'assets/icons/vehicle.svg', children: [] as {id:string,label:string}[] },
    { id: 'mqtt',      label: 'MQTT',      svg: 'assets/icons/mqtt.svg', children: [] as {id:string,label:string}[] },
    { id: 'routing',   label: 'Routing',   svg: 'assets/icons/routing.svg',
      children: [{id:'ports',label:'Port Forward'},{id:'dnat',label:'DNAT Target'},{id:'rules',label:'Firewall Rules'}] },
    { id: 'lwm2m',     label: 'LwM2M',     svg: 'assets/icons/lwm2m.svg',
      children: [{id:'server',label:'Server'},{id:'security',label:'Security'}] },
    { id: 'services',  label: 'Services',  svg: 'assets/icons/services.svg',
      children: [{id:'list',label:'All Services'}] },
    { id: 'logs',      label: 'Logs',      svg: 'assets/icons/logs.svg', children: [] as {id:string,label:string}[] },
    { id: 'users',     label: 'Users',     svg: 'assets/icons/users.svg', children: [] as {id:string,label:string}[] },
    { id: 'software',  label: 'Software',  svg: 'assets/icons/software.svg', children: [] as {id:string,label:string}[] },
    { id: 'shell',     label: 'Terminal',  svg: 'assets/icons/terminal.svg', children: [] as {id:string,label:string}[] },
    { id: 'advanced',  label: 'Advanced',  svg: 'assets/icons/advanced.svg',
      children: [{id:'reboot',label:'Reboot'},{id:'factory',label:'Factory Reset'},{id:'transfer',label:'Transfer'}] },
  ];

  expandedMenu: string | null = null;  // which menu is expanded in sidebar
  version = '';                         // running release (iot.version)
  shellEnabled = false;                 // http.shell.enabled (gates Terminal)

  get isAdmin(): boolean { return this.session.isAdmin; }

  // Terminal is a remote shell (runs as the iot-httpd service user, not root):
  // only surface it to Admins when the operator has switched on
  // http.shell.enabled. Everything else is always shown (each page enforces its
  // own access).
  get navMenus() {
    return this.menus.filter(m =>
      m.id !== 'shell' || (this.shellEnabled && this.isAdmin));
  }

  constructor(
    private http: HttpsvcService,
    private session: SessionService,
    private router: Router,
    private pubsub: PubSubService,
    public theme: ThemeService,
    public debug: DebugService,
    private ds: DataStoreService
  ) {
    this.theme.init();
    this.debug.init();
  }

  ngOnInit(): void {
    // Authenticated shell: warm the shared data-store cache once so every
    // config page paints instantly, and start watching for live changes.
    this.ds.prefetchAll();
    this.ds.startWatch();
    this.refreshStatus();
    // Running release version (written to ds by iot-httpd at startup).
    this.http.dbGet(['iot.version']).subscribe({
      next: (r) => {
        if (r.ok && r.data) this.version = String((r.data as Record<string, unknown>)['iot.version'] || '');
      }
    });
    // Terminal availability: observe http.shell.enabled off the shared store
    // (prefetched in ALL_KEYS) so toggling it on the HTTP Config page shows /
    // hides the Terminal sidebar item live, without a reload.
    this.ds.observe('http.shell.enabled').subscribe((v) => {
      this.shellEnabled = v === true || v === 'true';
    });
  }

  toggleMenu(menuId: string): void {
    if (this.expandedMenu === menuId) {
      this.expandedMenu = null;
    } else {
      this.expandedMenu = menuId;
    }
    this.selectedMenu = menuId;
    this.selectedSubNav = '';
    this.pubsub.publish('menu', menuId);
  }

  onChildSelect(menuId: string, childId: string): void {
    this.selectedMenu = menuId;
    this.selectedSubNav = childId;
    this.pubsub.publish('subnav', { menu: menuId, item: childId });
  }

  onMenuSelect(menuId: string): void {
    this.selectedMenu = menuId;
    this.selectedSubNav = '';
    this.pubsub.publish('menu', menuId);
  }

  onSubNavSelect(item: string): void {
    this.selectedSubNav = item;
    this.pubsub.publish('subnav', { menu: this.selectedMenu, item });
  }

  refreshStatus(): void {
    this.http.getStatus().subscribe({
      next: (s) => {
        this.status = s;
        this.pubsub.publish('status', s);
      }
    });
  }

  onLogout(): void {
    this.ds.stop();
    this.http.logout().subscribe({
      next: () => {
        this.session.clear();
        this.router.navigate(['/login']);
      },
      error: () => {
        this.session.clear();
        this.router.navigate(['/login']);
      }
    });
  }

  // Quick status helpers
  get vpnState(): string { return this.status?.vpn?.state || 'unknown'; }
  get wifiState(): string { return this.status?.wifi?.state || 'unknown'; }
  // LwM2M connection lifecycle (iot.conn.state → status.lwm2m.conn_state):
  // bootstrapping → bootstrapped → dm-connecting → dm-connected → registered.
  // The raw token is already concise enough for the live bar; map it to a
  // status-badge colour the same way the dashboard tile does.
  get lwm2mState(): string { return this.status?.lwm2m?.conn_state || 'idle'; }
  get lwm2mBadgeState(): string {
    const s = this.status?.lwm2m?.conn_state;
    if (s === 'registered') return 'connected';
    if (!s || s === 'idle' || s === 'failed') return 'disconnected';
    return 'starting';   // bootstrapping / bootstrapped / dm-* in progress
  }
  get wanIface(): string { return this.status?.wan?.active_iface || '-'; }
  /// Host uptime since boot (status.device.uptime_sec), humanised. Empty on
  /// older firmware that doesn't publish it → the top-bar stat hides via *ngIf.
  get deviceUptime(): string {
    const u = this.status?.device?.uptime_sec;
    return (u && u > 0) ? fmtDuration(u) : '';
  }
  get serviceCount(): number {
    if (!this.status?.services) return 0;
    let n = 0;
    for (const s of Object.values(this.status.services)) {
      if (s?.state === 'running') n++;
    }
    return n;
  }
}
