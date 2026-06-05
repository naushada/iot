import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-vpn-submenu',
  template: `
    <div class="subnav-bar">
      <a class="subnav-tab" [class.active]="active === 'config'"
         (click)="select('config')">
        <clr-icon shape="wrench"></clr-icon>
        <span>Configuration</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'status'"
         (click)="select('status')">
        <clr-icon shape="eye"></clr-icon>
        <span>Status</span>
      </a>
    </div>
  `,
  styles: [`
    .subnav-bar {
      display: flex; gap: 0; background: #fff;
      border-bottom: 2px solid #e0e0e0; padding: 0 24px;
    }
    .subnav-tab {
      display: flex; align-items: center; gap: 6px;
      padding: 12px 20px; cursor: pointer; font-size: 13px;
      color: #666; border-bottom: 2px solid transparent;
      margin-bottom: -2px; transition: all 0.15s;
    }
    .subnav-tab:hover { color: #333; background: #f5f5f5; }
    .subnav-tab.active { color: #2e7d32; border-bottom-color: #2e7d32; font-weight: 500; }
  `]
})
export class VpnSubmenuComponent {
  active = 'config';
  @Output() nav = new EventEmitter<string>();
  select(item: string): void { this.active = item; this.nav.emit(item); }
}
