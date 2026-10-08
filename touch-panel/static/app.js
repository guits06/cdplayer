const App = {
  pages: [PageCDPlayer],
  renderedPages: [false],
  currentIndex: 0,

  init() {
    this.switchPage(0);
  },

  switchPage(index) {
    if (index !== 0) return;
    const activePage = this.pages[0];
    const container = document.getElementById('page-0');
    if (container && activePage.render && !this.renderedPages[0]) {
      activePage.render(container);
      this.renderedPages[0] = true;
    }
  }
};

document.addEventListener('DOMContentLoaded', () => App.init());
