// The README demo letters in the languages that have their own screenshots.
// Japanese lives beside each English text in readme_screenshots.cpp.
#pragma once
#include <QHash>
#include <QString>

inline QHash<QString, QString> readmeDemoText(const QString &language) {
    if (language.startsWith("ko")) {
        return {
            {"Notes for Thursday",
             "목요일 메모"},
            {"Photos from Saturday",
             "토요일 사진"},
            {"Mesh networking notes",
             "메시 네트워크 소식"},
            {"Sounds good. I will bring the release checklist.",
             "좋아요. 릴리스 체크리스트를 가져갈게요."},
            {"Personal",
             "개인"},
            {"They came out great -- here is the best one:\n"
             "\n"
             "![Saturday at the beach][img1]\n"
             "\n"
             "More next week.",
             "정말 잘 나왔어요. 제일 좋은 한 장을 보내요:\n"
             "\n"
             "![토요일 바닷가][img1]\n"
             "\n"
             "나머지는 다음 주에 보낼게요."},
            {"Re: node on the Raspberry Pi",
             "Re: 라즈베리 파이 노드"},
            {"It has been up for nine days now and relays happily. Memory stays under 60 MB.",
             "벌써 9일째 잘 돌아가면서 문제없이 중계하고 있어요. 메모리는 60 MB 아래를 유지합니다."},
            {"Hi,\n"
             "\n"
             "Here is the plan for **Thursday**:\n"
             "\n"
             "## Agenda\n"
             "\n"
             "1. Walk through the new address book\n"
             "2. Decide on the release date\n"
             "3. Anything else you bring\n"
             "\n"
             "> Keep it short -- we have the room for an hour.\n"
             "\n"
             "See you there,\n"
             "Alice",
             "안녕하세요.\n"
             "\n"
             "**목요일** 일정입니다:\n"
             "\n"
             "## 안건\n"
             "\n"
             "1. 새 주소록 살펴보기\n"
             "2. 릴리스 날짜 정하기\n"
             "3. 그 밖에 가져오는 안건\n"
             "\n"
             "> 회의실은 한 시간뿐이니 짧게 해요.\n"
             "\n"
             "그때 봬요,\n"
             "지은"},
            {"Welcome to the general chan",
             "general 채널에 오신 것을 환영합니다"},
            {"Say hello, share what you are working on, and be kind.",
             "인사도 하고 하는 일도 나눠 주세요. 서로 친절하게요."},
            {"Anyone running ynotbit on Windows?",
             "Windows에서 ynotbit 쓰는 분 있나요?"},
            {"Curious how the new release behaves there.",
             "새 릴리스가 Windows에서 어떻게 동작하는지 궁금해요."},
            {"Yes -- the network engine is the same as on Linux now. Works well.",
             "네, 이제 네트워크 엔진이 Linux와 같아요. 잘 돌아갑니다."},
            {"A month on a solar-powered relay",
             "태양광 중계 노드로 보낸 한 달"},
            {"The Raspberry Pi node has now run for **31 days** on a 20 W panel.\n"
             "\n"
             "- Uptime: 99.2%, two short stops on cloudy mornings\n"
             "- Memory: under 60 MB the whole time\n"
             "- Objects relayed: about 14,000 a day\n"
             "\n"
             "Next: a second node at the allotment.",
             "라즈베리 파이 노드가 20 W 패널로 **31일** 동안 돌아갔습니다.\n"
             "\n"
             "- 가동률: 99.2%, 흐린 아침에 두 번 잠깐 멈춤\n"
             "- 메모리: 내내 60 MB 미만\n"
             "- 중계한 객체: 하루 약 14,000개\n"
             "\n"
             "다음 목표: 주말농장에 두 번째 노드."},
            {"Bitmessage over Tor, revisited",
             "Tor를 통한 Bitmessage, 다시 보기"},
            {"Routing the node through a local Tor proxy still works well.\n"
             "\n"
             "## Two tips\n"
             "\n"
             "- Expect slower first contact with peers, then normal traffic\n"
             "- Keep incoming connections off when running behind Tor",
             "로컬 Tor 프록시를 거쳐도 노드는 여전히 잘 동작합니다.\n"
             "\n"
             "## 두 가지 팁\n"
             "\n"
             "- 피어와의 첫 연결은 느리지만, 그 뒤로는 평소와 같습니다\n"
             "- Tor 뒤에서는 들어오는 연결을 꺼 두세요"},
            {"Reading list",
             "읽을거리"},
            {"## This week\n"
             "\n"
             "1. How proof of work keeps the network quiet\n"
             "2. Chans: shared addresses, shared keys\n"
             "3. Why every message reaches every node",
             "## 이번 주\n"
             "\n"
             "1. 작업 증명이 네트워크를 조용하게 지키는 방법\n"
             "2. 채널: 공유 주소, 공유 키\n"
             "3. 모든 메시지가 모든 노드에 닿는 이유"},
            {"Alice Liddell",
             "김지은"},
            {"Bob",
             "박민수"},
            {"Carol",
             "이서연"},
            {"Thursday",
             "목요일"},
            {"Looking forward to it -- see you at ten.",
             "기대할게요. 10시에 봬요."},
        };
    } else if (language == "zh_CN") {
        return {
            {"Notes for Thursday",
             "周四的笔记"},
            {"Photos from Saturday",
             "周六的照片"},
            {"Mesh networking notes",
             "网状网络笔记"},
            {"Sounds good. I will bring the release checklist.",
             "好的，我会带上发布清单。"},
            {"Personal",
             "私人"},
            {"They came out great -- here is the best one:\n"
             "\n"
             "![Saturday at the beach][img1]\n"
             "\n"
             "More next week.",
             "照片拍得很好，这是最好的一张：\n"
             "\n"
             "![周六在海边][img1]\n"
             "\n"
             "其余的下周发给你。"},
            {"Re: node on the Raspberry Pi",
             "Re: 树莓派上的节点"},
            {"It has been up for nine days now and relays happily. Memory stays under 60 MB.",
             "已经连续运行九天了，转发一切正常。内存一直低于 60 MB。"},
            {"Hi,\n"
             "\n"
             "Here is the plan for **Thursday**:\n"
             "\n"
             "## Agenda\n"
             "\n"
             "1. Walk through the new address book\n"
             "2. Decide on the release date\n"
             "3. Anything else you bring\n"
             "\n"
             "> Keep it short -- we have the room for an hour.\n"
             "\n"
             "See you there,\n"
             "Alice",
             "你好，\n"
             "\n"
             "这是**周四**的安排：\n"
             "\n"
             "## 议程\n"
             "\n"
             "1. 过一遍新的通讯录\n"
             "2. 确定发布日期\n"
             "3. 你带来的其他议题\n"
             "\n"
             "> 会议室只有一个小时，尽量简短。\n"
             "\n"
             "到时见，\n"
             "丽"},
            {"Welcome to the general chan",
             "欢迎来到 general 频道"},
            {"Say hello, share what you are working on, and be kind.",
             "打个招呼，聊聊你在做什么，请友善相待。"},
            {"Anyone running ynotbit on Windows?",
             "有人在 Windows 上用 ynotbit 吗？"},
            {"Curious how the new release behaves there.",
             "想知道新版本在那边表现如何。"},
            {"Yes -- the network engine is the same as on Linux now. Works well.",
             "有的，现在网络引擎和 Linux 上的一样了，运行良好。"},
            {"A month on a solar-powered relay",
             "太阳能中继节点的一个月"},
            {"The Raspberry Pi node has now run for **31 days** on a 20 W panel.\n"
             "\n"
             "- Uptime: 99.2%, two short stops on cloudy mornings\n"
             "- Memory: under 60 MB the whole time\n"
             "- Objects relayed: about 14,000 a day\n"
             "\n"
             "Next: a second node at the allotment.",
             "树莓派节点靠一块 20 W 的太阳能板已经运行了 **31 天**。\n"
             "\n"
             "- 在线率：99.2%，阴天早晨短暂停机两次\n"
             "- 内存：始终低于 60 MB\n"
             "- 转发的对象：每天约 14,000 个\n"
             "\n"
             "下一步：在菜园里再放一个节点。"},
            {"Bitmessage over Tor, revisited",
             "再谈通过 Tor 使用 Bitmessage"},
            {"Routing the node through a local Tor proxy still works well.\n"
             "\n"
             "## Two tips\n"
             "\n"
             "- Expect slower first contact with peers, then normal traffic\n"
             "- Keep incoming connections off when running behind Tor",
             "通过本地 Tor 代理运行节点依然很好用。\n"
             "\n"
             "## 两个建议\n"
             "\n"
             "- 首次连接对等节点会慢一些，之后流量正常\n"
             "- 在 Tor 后面运行时请关闭传入连接"},
            {"Reading list",
             "阅读清单"},
            {"## This week\n"
             "\n"
             "1. How proof of work keeps the network quiet\n"
             "2. Chans: shared addresses, shared keys\n"
             "3. Why every message reaches every node",
             "## 本周\n"
             "\n"
             "1. 工作量证明如何让网络保持安静\n"
             "2. 频道：共享地址，共享密钥\n"
             "3. 为什么每条消息都会到达每个节点"},
            {"Alice Liddell",
             "王丽"},
            {"Bob",
             "李明"},
            {"Carol",
             "陈静"},
            {"Thursday",
             "周四"},
            {"Looking forward to it -- see you at ten.",
             "很期待，十点见。"},
        };
    } else if (language == "zh_TW") {
        return {
            {"Notes for Thursday",
             "週四的筆記"},
            {"Photos from Saturday",
             "週六的照片"},
            {"Mesh networking notes",
             "網狀網路筆記"},
            {"Sounds good. I will bring the release checklist.",
             "好的，我會帶上發布清單。"},
            {"Personal",
             "私人"},
            {"They came out great -- here is the best one:\n"
             "\n"
             "![Saturday at the beach][img1]\n"
             "\n"
             "More next week.",
             "照片拍得很好，這是最好的一張：\n"
             "\n"
             "![週六在海邊][img1]\n"
             "\n"
             "其餘的下週再寄給你。"},
            {"Re: node on the Raspberry Pi",
             "Re: 樹莓派上的節點"},
            {"It has been up for nine days now and relays happily. Memory stays under 60 MB.",
             "已經連續運作九天了，轉送一切正常。記憶體一直低於 60 MB。"},
            {"Hi,\n"
             "\n"
             "Here is the plan for **Thursday**:\n"
             "\n"
             "## Agenda\n"
             "\n"
             "1. Walk through the new address book\n"
             "2. Decide on the release date\n"
             "3. Anything else you bring\n"
             "\n"
             "> Keep it short -- we have the room for an hour.\n"
             "\n"
             "See you there,\n"
             "Alice",
             "你好，\n"
             "\n"
             "這是**週四**的安排：\n"
             "\n"
             "## 議程\n"
             "\n"
             "1. 看一遍新的通訊錄\n"
             "2. 決定發布日期\n"
             "3. 你帶來的其他議題\n"
             "\n"
             "> 會議室只有一個小時，盡量簡短。\n"
             "\n"
             "到時見，\n"
             "怡君"},
            {"Welcome to the general chan",
             "歡迎來到 general 頻道"},
            {"Say hello, share what you are working on, and be kind.",
             "打聲招呼，聊聊你在做什麼，請友善相待。"},
            {"Anyone running ynotbit on Windows?",
             "有人在 Windows 上用 ynotbit 嗎？"},
            {"Curious how the new release behaves there.",
             "想知道新版本在那邊表現如何。"},
            {"Yes -- the network engine is the same as on Linux now. Works well.",
             "有的，現在網路引擎和 Linux 上的一樣了，運作良好。"},
            {"A month on a solar-powered relay",
             "太陽能中繼節點的一個月"},
            {"The Raspberry Pi node has now run for **31 days** on a 20 W panel.\n"
             "\n"
             "- Uptime: 99.2%, two short stops on cloudy mornings\n"
             "- Memory: under 60 MB the whole time\n"
             "- Objects relayed: about 14,000 a day\n"
             "\n"
             "Next: a second node at the allotment.",
             "樹莓派節點靠一塊 20 W 的太陽能板已經運作了 **31 天**。\n"
             "\n"
             "- 上線率：99.2%，陰天早晨短暫停機兩次\n"
             "- 記憶體：始終低於 60 MB\n"
             "- 轉送的物件：每天約 14,000 個\n"
             "\n"
             "下一步：在菜園裡再放一個節點。"},
            {"Bitmessage over Tor, revisited",
             "再談透過 Tor 使用 Bitmessage"},
            {"Routing the node through a local Tor proxy still works well.\n"
             "\n"
             "## Two tips\n"
             "\n"
             "- Expect slower first contact with peers, then normal traffic\n"
             "- Keep incoming connections off when running behind Tor",
             "透過本機 Tor 代理伺服器執行節點依然很好用。\n"
             "\n"
             "## 兩個建議\n"
             "\n"
             "- 第一次連線到對等節點會慢一些，之後流量正常\n"
             "- 在 Tor 後面執行時請關閉連入連線"},
            {"Reading list",
             "閱讀清單"},
            {"## This week\n"
             "\n"
             "1. How proof of work keeps the network quiet\n"
             "2. Chans: shared addresses, shared keys\n"
             "3. Why every message reaches every node",
             "## 本週\n"
             "\n"
             "1. 工作量證明如何讓網路保持安靜\n"
             "2. 頻道：共用位址，共用金鑰\n"
             "3. 為什麼每則訊息都會到達每個節點"},
            {"Alice Liddell",
             "林怡君"},
            {"Bob",
             "陳志明"},
            {"Carol",
             "黃雅婷"},
            {"Thursday",
             "週四"},
            {"Looking forward to it -- see you at ten.",
             "很期待，十點見。"},
        };
    } else if (language.startsWith("ru")) {
        return {
            {"Notes for Thursday",
             "Заметки на четверг"},
            {"Photos from Saturday",
             "Фото с субботы"},
            {"Mesh networking notes",
             "Mesh-сети"},
            {"Sounds good. I will bring the release checklist.",
             "Отлично. Принесу чек-лист релиза."},
            {"Personal",
             "Личное"},
            {"They came out great -- here is the best one:\n"
             "\n"
             "![Saturday at the beach][img1]\n"
             "\n"
             "More next week.",
             "Получилось здорово — вот лучшее:\n"
             "\n"
             "![Суббота на пляже][img1]\n"
             "\n"
             "Остальные пришлю на следующей неделе."},
            {"Re: node on the Raspberry Pi",
             "Re: узел на Raspberry Pi"},
            {"It has been up for nine days now and relays happily. Memory stays under 60 MB.",
             "Работает уже девять дней и исправно ретранслирует. Память держится ниже 60 МБ."},
            {"Hi,\n"
             "\n"
             "Here is the plan for **Thursday**:\n"
             "\n"
             "## Agenda\n"
             "\n"
             "1. Walk through the new address book\n"
             "2. Decide on the release date\n"
             "3. Anything else you bring\n"
             "\n"
             "> Keep it short -- we have the room for an hour.\n"
             "\n"
             "See you there,\n"
             "Alice",
             "Привет!\n"
             "\n"
             "План на **четверг**:\n"
             "\n"
             "## Повестка\n"
             "\n"
             "1. Пройтись по новой адресной книге\n"
             "2. Решить с датой релиза\n"
             "3. Всё, что принесёшь сам\n"
             "\n"
             "> Покороче — переговорка у нас на час.\n"
             "\n"
             "До встречи,\n"
             "Анна"},
            {"Welcome to the general chan",
             "Добро пожаловать в канал general"},
            {"Say hello, share what you are working on, and be kind.",
             "Поздоровайтесь, расскажите, над чем работаете, и будьте добры друг к другу."},
            {"Anyone running ynotbit on Windows?",
             "Кто-нибудь запускает ynotbit на Windows?"},
            {"Curious how the new release behaves there.",
             "Интересно, как там ведёт себя новый релиз."},
            {"Yes -- the network engine is the same as on Linux now. Works well.",
             "Да — сетевой движок теперь тот же, что и на Linux. Работает хорошо."},
            {"A month on a solar-powered relay",
             "Месяц на ретрансляторе с солнечной панелью"},
            {"The Raspberry Pi node has now run for **31 days** on a 20 W panel.\n"
             "\n"
             "- Uptime: 99.2%, two short stops on cloudy mornings\n"
             "- Memory: under 60 MB the whole time\n"
             "- Objects relayed: about 14,000 a day\n"
             "\n"
             "Next: a second node at the allotment.",
             "Узел на Raspberry Pi проработал **31 день** от панели на 20 Вт.\n"
             "\n"
             "- Время работы: 99,2%, две короткие остановки пасмурным утром\n"
             "- Память: всё время меньше 60 МБ\n"
             "- Ретранслировано объектов: около 14 000 в день\n"
             "\n"
             "Дальше: второй узел на даче."},
            {"Bitmessage over Tor, revisited",
             "Bitmessage через Tor: снова"},
            {"Routing the node through a local Tor proxy still works well.\n"
             "\n"
             "## Two tips\n"
             "\n"
             "- Expect slower first contact with peers, then normal traffic\n"
             "- Keep incoming connections off when running behind Tor",
             "Узел через локальный прокси Tor по-прежнему работает хорошо.\n"
             "\n"
             "## Два совета\n"
             "\n"
             "- Первое соединение с пирами медленнее, дальше трафик обычный\n"
             "- За Tor держите входящие соединения выключенными"},
            {"Reading list",
             "Что почитать"},
            {"## This week\n"
             "\n"
             "1. How proof of work keeps the network quiet\n"
             "2. Chans: shared addresses, shared keys\n"
             "3. Why every message reaches every node",
             "## На этой неделе\n"
             "\n"
             "1. Как доказательство работы удерживает сеть от спама\n"
             "2. Каналы: общие адреса, общие ключи\n"
             "3. Почему каждое сообщение доходит до каждого узла"},
            {"Alice Liddell",
             "Анна Смирнова"},
            {"Bob",
             "Борис"},
            {"Carol",
             "Катя"},
            {"Thursday",
             "Четверг"},
            {"Looking forward to it -- see you at ten.",
             "Жду с нетерпением — увидимся в десять."},
        };
    } else if (language.startsWith("uk")) {
        return {
            {"Notes for Thursday",
             "Нотатки на четвер"},
            {"Photos from Saturday",
             "Фото із суботи"},
            {"Mesh networking notes",
             "Mesh-мережі"},
            {"Sounds good. I will bring the release checklist.",
             "Чудово. Принесу чек-лист релізу."},
            {"Personal",
             "Особисте"},
            {"They came out great -- here is the best one:\n"
             "\n"
             "![Saturday at the beach][img1]\n"
             "\n"
             "More next week.",
             "Вийшло чудово — ось найкраще:\n"
             "\n"
             "![Субота на пляжі][img1]\n"
             "\n"
             "Решту надішлю наступного тижня."},
            {"Re: node on the Raspberry Pi",
             "Re: вузол на Raspberry Pi"},
            {"It has been up for nine days now and relays happily. Memory stays under 60 MB.",
             "Працює вже дев'ять днів і справно ретранслює. Пам'ять тримається нижче 60 МБ."},
            {"Hi,\n"
             "\n"
             "Here is the plan for **Thursday**:\n"
             "\n"
             "## Agenda\n"
             "\n"
             "1. Walk through the new address book\n"
             "2. Decide on the release date\n"
             "3. Anything else you bring\n"
             "\n"
             "> Keep it short -- we have the room for an hour.\n"
             "\n"
             "See you there,\n"
             "Alice",
             "Привіт!\n"
             "\n"
             "План на **четвер**:\n"
             "\n"
             "## Порядок денний\n"
             "\n"
             "1. Переглянути нову адресну книгу\n"
             "2. Вирішити з датою релізу\n"
             "3. Усе, що принесеш сам\n"
             "\n"
             "> Коротше — переговорна в нас на годину.\n"
             "\n"
             "До зустрічі,\n"
             "Олена"},
            {"Welcome to the general chan",
             "Ласкаво просимо до каналу general"},
            {"Say hello, share what you are working on, and be kind.",
             "Привітайтеся, розкажіть, над чим працюєте, і будьте добрі одне до одного."},
            {"Anyone running ynotbit on Windows?",
             "Хтось запускає ynotbit на Windows?"},
            {"Curious how the new release behaves there.",
             "Цікаво, як там поводиться новий реліз."},
            {"Yes -- the network engine is the same as on Linux now. Works well.",
             "Так — мережевий рушій тепер той самий, що й на Linux. Працює добре."},
            {"A month on a solar-powered relay",
             "Місяць на ретрансляторі із сонячною панеллю"},
            {"The Raspberry Pi node has now run for **31 days** on a 20 W panel.\n"
             "\n"
             "- Uptime: 99.2%, two short stops on cloudy mornings\n"
             "- Memory: under 60 MB the whole time\n"
             "- Objects relayed: about 14,000 a day\n"
             "\n"
             "Next: a second node at the allotment.",
             "Вузол на Raspberry Pi пропрацював **31 день** від панелі на 20 Вт.\n"
             "\n"
             "- Час роботи: 99,2%, дві короткі зупинки хмарного ранку\n"
             "- Пам'ять: увесь час менше 60 МБ\n"
             "- Ретрансльовано об'єктів: близько 14 000 на день\n"
             "\n"
             "Далі: другий вузол на дачі."},
            {"Bitmessage over Tor, revisited",
             "Bitmessage через Tor: знову"},
            {"Routing the node through a local Tor proxy still works well.\n"
             "\n"
             "## Two tips\n"
             "\n"
             "- Expect slower first contact with peers, then normal traffic\n"
             "- Keep incoming connections off when running behind Tor",
             "Вузол через локальний проксі Tor і досі працює добре.\n"
             "\n"
             "## Дві поради\n"
             "\n"
             "- Перше з'єднання з пірами повільніше, далі трафік звичайний\n"
             "- За Tor тримайте вхідні з'єднання вимкненими"},
            {"Reading list",
             "Що почитати"},
            {"## This week\n"
             "\n"
             "1. How proof of work keeps the network quiet\n"
             "2. Chans: shared addresses, shared keys\n"
             "3. Why every message reaches every node",
             "## Цього тижня\n"
             "\n"
             "1. Як доказ роботи захищає мережу від спаму\n"
             "2. Канали: спільні адреси, спільні ключі\n"
             "3. Чому кожне повідомлення доходить до кожного вузла"},
            {"Alice Liddell",
             "Олена Коваленко"},
            {"Bob",
             "Богдан"},
            {"Carol",
             "Катерина"},
            {"Thursday",
             "Четвер"},
            {"Looking forward to it -- see you at ten.",
             "Чекаю з нетерпінням — побачимося о десятій."},
        };
    }
    return {};
}
