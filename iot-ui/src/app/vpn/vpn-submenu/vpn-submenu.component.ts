import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-vpn-submenu',
  template: `
    <nav class="subnav">
      <a class="subnav-item" [class.active]="active === 'config'"
         (click)="select('config')">Configuration</a>
      <a class="subnav-item" [class.active]="active === 'status'"
         (click)="select('status')">Status</a>
    </nav>
  `,
  styles: [`
    .subnav { display:flex; gap:0; background:#16213e; padding:0 1rem; border-bottom:1px solid #1a5276; }
    .subnav-item { padding:10px 16px; color:#bdbdbd; cursor:pointer; font-size:13px; border-bottom:2px solid transparent; }
    .subnav-item:hover { color:#e0e0e0; }
    .subnav-item.active { color:#66bb6a; border-bottom-color:#2e7d32; }
  `]
})
export class VpnSubmenuComponent {
  active = 'config';
  @Output() nav = new EventEmitter<string>();

  select(item: string): void { this.active = item; this.nav.emit(item); }
}
