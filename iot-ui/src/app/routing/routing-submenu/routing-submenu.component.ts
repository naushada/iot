import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-routing-submenu',
  template: `
    <nav class="subnav">
      <a class="subnav-item" [class.active]="active==='routes'" (click)="s('routes')">Routes</a>
      <a class="subnav-item" [class.active]="active==='ports'" (click)="s('ports')">Port Forward</a>
      <a class="subnav-item" [class.active]="active==='dnat'" (click)="s('dnat')">DNAT Target</a>
      <a class="subnav-item" [class.active]="active==='rules'" (click)="s('rules')">Firewall Rules</a>
    </nav>
  `,
  styles: [`
    .subnav { display:flex; background:#16213e; padding:0 1rem; border-bottom:1px solid #1a5276; }
    .subnav-item { padding:10px 16px; color:#bdbdbd; cursor:pointer; font-size:13px; border-bottom:2px solid transparent; }
    .subnav-item:hover { color:#e0e0e0; }
    .subnav-item.active { color:#66bb6a; border-bottom-color:#2e7d32; }
  `]
})
export class RoutingSubmenuComponent {
  active = 'ports';
  @Output() nav = new EventEmitter<string>();
  s(item: string): void { this.active = item; this.nav.emit(item); }
}
