import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-routing-submenu',
  template: `
    <div class="subnav-bar">
      <a class="subnav-tab" [class.active]="active === 'ports'"
         (click)="select('ports')">Port Forward</a>
      <a class="subnav-tab" [class.active]="active === 'dnat'"
         (click)="select('dnat')">DNAT Target</a>
      <a class="subnav-tab" [class.active]="active === 'rules'"
         (click)="select('rules')">Firewall Rules</a>
    </div>
  `,
  // styles provided by global .subnav-bar / .subnav-tab in styles.scss
})
export class RoutingSubmenuComponent {
  active = 'ports';
  @Output() nav = new EventEmitter<string>();
  select(item: string): void { this.active = item; this.nav.emit(item); }
}
