import { Component, Output, EventEmitter } from '@angular/core';

@Component({
  selector: 'app-advanced-submenu',
  template: `
    <div class="subnav-bar">
      <a class="subnav-tab" [class.active]="active === 'reboot'"
         (click)="select('reboot')">
        <clr-icon shape="power"></clr-icon>
        <span>Reboot</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'factory'"
         (click)="select('factory')">
        <clr-icon shape="trash"></clr-icon>
        <span>Factory Reset</span>
      </a>
      <a class="subnav-tab" [class.active]="active === 'transfer'"
         (click)="select('transfer')">
        <clr-icon shape="two-way-arrows"></clr-icon>
        <span>Transfer</span>
      </a>
    </div>
  `,
  // styles provided by global .subnav-bar / .subnav-tab in styles.scss
})
export class AdvancedSubmenuComponent {
  active = 'reboot';
  @Output() nav = new EventEmitter<string>();
  select(item: string): void { this.active = item; this.nav.emit(item); }
}
