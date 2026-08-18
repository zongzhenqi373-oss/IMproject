package com.jitong.im.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FriendListScreen(vm: MainViewModel) {
    val myNick by vm.myNick.collectAsStateWithLifecycle()
    val myFeeling by vm.myFeeling.collectAsStateWithLifecycle()
    val friends by vm.friends.collectAsStateWithLifecycle()
    val conversations by vm.conversations.collectAsStateWithLifecycle()
    val results by vm.searchResults.collectAsStateWithLifecycle()
    val myId = vm.myId

    var showProfile by remember { mutableStateOf(false) }
    var query by rememberSaveable { mutableStateOf("") }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("即通") },
                actions = {
                    TextButton(onClick = { vm.logout() }) { Text("下线") }
                },
            )
        },
    ) { padding ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(padding)
                .background(Color(0xFFF5F5F5)),
        ) {
            // 我的资料卡入口
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color.White)
                    .clickable { showProfile = true }
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Avatar(id = myId, nick = myNick, size = 52.dp)
                Spacer(Modifier.width(12.dp))
                Column {
                    Text(myNick, style = MaterialTheme.typography.titleMedium)
                    Text(myFeeling, style = MaterialTheme.typography.bodySmall, color = Color.Gray)
                }
            }
            Spacer(Modifier.height(8.dp))

            // 聊天记录搜索（FTS4 + LIKE 双路）
            OutlinedTextField(
                value = query,
                onValueChange = {
                    query = it
                    vm.search(it)
                },
                placeholder = { Text("搜索聊天记录", fontSize = 14.sp) },
                singleLine = true,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp, vertical = 4.dp)
                    .background(Color.White),
            )

            if (query.isNotBlank()) {
                // 搜索结果
                LazyColumn(Modifier.background(Color.White)) {
                    items(results, key = { it.msgId }) { m ->
                        val friend = friends.firstOrNull { it.id == m.peerId } ?: return@items
                        SearchResultRow(
                            friend = friend,
                            content = m.content.orEmpty(),
                            ts = m.ts,
                            onClick = {
                                query = ""
                                vm.openChat(friend)
                            },
                        )
                        HorizontalDivider(
                            modifier = Modifier.padding(start = 76.dp),
                            thickness = 0.5.dp,
                            color = Color(0xFFE0E0E0),
                        )
                    }
                }
            } else if (friends.isEmpty()) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text("暂无好友（可用 im_cli 或另一台模拟器互加）", color = Color.Gray)
                }
            } else {
                LazyColumn(Modifier.background(Color.White)) {
                    items(friends, key = { it.id }) { friend ->
                        FriendRow(
                            friend = friend,
                            lastMsg = conversations[friend.id]?.lastMsg,
                            unread = conversations[friend.id]?.unread ?: 0,
                            onClick = { vm.openChat(friend) },
                        )
                        HorizontalDivider(
                            modifier = Modifier.padding(start = 76.dp),
                            thickness = 0.5.dp,
                            color = Color(0xFFE0E0E0),
                        )
                    }
                }
            }
        }
    }

    if (showProfile) {
        AlertDialog(
            onDismissRequest = { showProfile = false },
            confirmButton = {
                TextButton(onClick = { showProfile = false }) { Text("关闭") }
            },
            title = { Text("我的资料") },
            text = {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Avatar(id = myId, nick = myNick, size = 64.dp)
                    Spacer(Modifier.width(16.dp))
                    Column {
                        Text(myNick, style = MaterialTheme.typography.titleLarge)
                        Spacer(Modifier.height(4.dp))
                        Text("账号：$myId", color = Color.Gray)
                        Text("签名：$myFeeling", color = Color.Gray)
                    }
                }
            },
        )
    }
}

@Composable
private fun FriendRow(friend: Friend, lastMsg: String?, unread: Int, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box {
            Avatar(id = friend.id, nick = friend.nick, size = 52.dp)
            Box(
                Modifier
                    .size(14.dp)
                    .align(Alignment.BottomEnd)
                    .background(Color.White, CircleShape)
                    .padding(2.dp)
                    .background(
                        if (friend.online) Color(0xFF07C160) else Color(0xFFBFBFBF),
                        CircleShape,
                    )
            )
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(friend.nick, style = MaterialTheme.typography.titleMedium)
            Text(
                lastMsg ?: if (friend.online) "在线 · ${friend.feeling}" else "离线",
                style = MaterialTheme.typography.bodySmall,
                color = Color.Gray,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        if (unread > 0) {
            Spacer(Modifier.width(8.dp))
            Box(
                Modifier
                    .size(20.dp)
                    .background(Color(0xFFF43530), CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    if (unread > 99) "99+" else unread.toString(),
                    color = Color.White,
                    fontSize = 11.sp,
                )
            }
        }
    }
}

@Composable
private fun SearchResultRow(friend: Friend, content: String, ts: Long, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Avatar(id = friend.id, nick = friend.nick, size = 44.dp)
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                Text(friend.nick, style = MaterialTheme.typography.titleSmall)
                Text(formatTs(ts), fontSize = 11.sp, color = Color.Gray)
            }
            Text(
                content,
                style = MaterialTheme.typography.bodySmall,
                color = Color.Gray,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

private fun formatTs(ts: Long): String {
    val now = System.currentTimeMillis()
    val sameDay = SimpleDateFormat("yyyyMMdd", Locale.getDefault()).format(Date(ts)) ==
        SimpleDateFormat("yyyyMMdd", Locale.getDefault()).format(Date(now))
    return SimpleDateFormat(if (sameDay) "HH:mm" else "MM-dd", Locale.getDefault()).format(Date(ts))
}
